#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/uaccess.h> //copy_to/from_user()
#include <linux/gpio.h> //GPIO
#include <linux/types.h>
//LED is connected to this GPIO
#define GPIO_14 (14)  //a
#define GPIO_15 (15)  //b
#define GPIO_18 (18)  //c
#define GPIO_23 (23)  //d)
#define GPIO_24 (24)  //e
#define GPIO_22 (22)  //f
#define GPIO_17 (17)  //g

dev_t dev = 0;
static struct class *dev_class;
static struct cdev etx_cdev;
static int __init etx_driver_init(void);
static void __exit etx_driver_exit(void);
static int seg_pins[7]={GPIO_14,GPIO_15,GPIO_18,GPIO_23,GPIO_24,GPIO_22,GPIO_17};
static const  unsigned char seg_map[11]={
    0x3F, /*0: abcdef      */
    0x06, /*1:   bc        */
    0x5B, /*2: ab de g     */
    0x4F, /*3: abcd  g     */
    0x66, /*4:  f gb c     */
    0x6D, /*5: a cde fg    */
    0x7D, /*6: a cdefg     */
    0x07, /*7: abc         */
    0x7F, /*8: abcdefg     */
    0x6F,  /*9: abcd fg     */
    0x79   /*E:adefg     */       
};

static unsigned char  last_mask;
static char last_digit = '?';
/*************** Driver functions **********************/
static int etx_open(struct inode *inode, struct file *file);
static int etx_release(struct inode *inode, struct file *file);
static ssize_t etx_read(struct file *filp,char __user *buf, size_t len,loff_t * off);
static ssize_t etx_write(struct file *filp,const char __user *buf, size_t len, loff_t * off);
/******************************************************/
//File operation structure
static struct file_operations fops =
{
    .owner = THIS_MODULE,
    .read = etx_read,
    .write = etx_write,
    .open = etx_open,
    .release = etx_release,
};

static void gpio_on(unsigned char m)
{
    int i;
    for( i=0;i<7;i++)
    {
        int on=(m>>i)&1;
        gpio_set_value(seg_pins[i],on);
    }
    last_mask = m;
}
/*
** This function will be called when we open the Device file
*/
static int etx_open(struct inode *inode, struct file *file)
{
    pr_info("Device File Opened...!!!\n");
    return 0;
}
/*
** This function will be called when we close the Device file
*/
static int etx_release(struct inode *inode, struct file *file)
{
    pr_info("Device File Closed...!!!\n");
    return 0;
}
/*
** This function will be called when we read the Device file
*/
static ssize_t etx_read(struct file *filp,char __user *buf, size_t len, loff_t *off)
{
    char out=last_digit;
//write to user
    if (*off > 0) return 0;
    if( copy_to_user(buf, &out, 1) > 0) {
        pr_err("ERROR: Not all the bytes have been copied to user\n");
    }
    *off = 1;
    return 1;
}

static ssize_t etx_write(struct file *filp, const char __user *buf, size_t len, loff_t *off)
{
    u8 k_buf[32];
    size_t n = (len < sizeof(k_buf)-1) ? len : sizeof(k_buf)-1;

    if (copy_from_user(k_buf, buf, n) > 0)
        return -EFAULT;

    k_buf[n] = '\0';

    size_t ok = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = k_buf[i];
        if (c == '\n' || c == '\r')
            continue;
        if (c >= '0' && c <= '9') {
            unsigned char mask = seg_map[c-'0'];
            gpio_on(mask);
            last_digit = c;
            pr_info("seg7: show %c (mask=0x%02x)\n", c, mask);
            ok++;
        } else if(c=='E')
        {
            unsigned char mask = seg_map[10];
            gpio_on(mask);
            last_digit = c;
            pr_info("seg7: show %c (mask=0x%02x)\n", c, mask);
            ok++;
        }else {
            pr_err("seg7: invalid char '%c'\n", c ? c : '?');
            return -EINVAL;
        }
    }
    return ok ? (ssize_t)len : -EINVAL;
}
/*
** Module Init function
*/
static int __init etx_driver_init(void)
{
    
/*Allocating Major number*/
    if((alloc_chrdev_region(&dev, 0, 1, "etx_Dev")) <0){
        pr_err("Cannot allocate major number\n");
        goto r_unreg;
    }
    pr_info("Major = %d Minor = %d \n",MAJOR(dev), MINOR(dev));
/*Creating cdev structure*/
    cdev_init(&etx_cdev,&fops);
/*Adding character device to the system*/
    if((cdev_add(&etx_cdev,dev,1)) < 0){
        pr_err("Cannot add the device to the system\n");
        goto r_del;
    }
/*Creating struct class*/
    if((dev_class = class_create(THIS_MODULE,"etx_class")) == NULL){
        pr_err("Cannot create the struct class\n");
        goto r_class;
    }
/*Creating device*/
    if((device_create(dev_class,NULL,dev,NULL,"etx_device")) == NULL){
        pr_err( "Cannot create the Device \n");
        goto r_class;
    }
//Checking the GPIO is valid or not
    int i;
    for (i = 0; i < 7; ++i) {
        if (!gpio_is_valid(seg_pins[i])) { 
            pr_err("GPIO %d invalid\n", seg_pins[i]);
            goto r_gpio; 
        }
        if (gpio_request(seg_pins[i], "seg7")) {
            pr_err("GPIO %d request fail\n", seg_pins[i]); 
            goto r_gpio; 
        }
        gpio_direction_output(seg_pins[i], 0);
    }
    gpio_on(0x00); 
    last_digit = '?';

/* Using this call the GPIO 21 will be visible in /sys/class/gpio/
** Now you can change the gpio values by using below commands also.
** echo 1 > /sys/class/gpio/gpio21/value (turn ON the LED)
** echo 0 > /sys/class/gpio/gpio21/value (turn OFF the LED)
** cat /sys/class/gpio/gpio21/value (read the value LED)
**
** the second argument prevents the direction from being changed.
*/
    pr_info("Device Driver Insert...Done!!!\n");
    return 0;
    r_gpio:
    while(--i>=0)
    {
        gpio_set_value(seg_pins[i], 0);
        gpio_free(seg_pins[i]);
    }
    device_destroy(dev_class, dev);
    r_class:
    class_destroy(dev_class);
    r_del:
    cdev_del(&etx_cdev);
    r_unreg:
    unregister_chrdev_region(dev,1);
    return -1;
}
/*
** Module exit function
*/
static void __exit etx_driver_exit(void)
{
    int i;
    for ( i = 0; i < 7; ++i) {
    // gpio_unexport(seg_pins[i]);
        gpio_set_value(seg_pins[i], 0);
        gpio_free(seg_pins[i]);
    }
    device_destroy(dev_class,dev);
    class_destroy(dev_class);
    cdev_del(&etx_cdev);
    unregister_chrdev_region(dev, 1);
    pr_info("Device Driver Remove...Done!!\n");
}
module_init(etx_driver_init);
module_exit(etx_driver_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("EmbeTronicX <embetronicx@gmail.com>");
MODULE_DESCRIPTION("A simple device driver - GPIO Driver");
MODULE_VERSION("1.32");

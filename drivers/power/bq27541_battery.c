/*
 * BQ27510 battery driver
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */
#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <asm/unaligned.h>
#include <mach/gpio.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <mach/board.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <mach/iomux.h>


#define DRIVER_VERSION			"1.1.0"
#define BQ27x00_REG_TEMP		0x06
#define BQ27x00_REG_VOLT		0x08
#define BQ27x00_REG_RM		    0x10
#define BQ27x00_REG_AI			0x14
#define BQ27x00_REG_FLAGS		0x0A
#define BQ27x00_REG_TTE			0x16
#define BQ27x00_REG_TTF			0x18
#define BQ27x00_REG_TTECP		0x26
#define BQ27000_REG_RSOC		0x0B /* Relative State-of-Charge */
#define BQ27500_REG_SOC			0x2c

#define BQ27500_FLAG_DSC		BIT(0)
#define BQ27500_FLAG_LOW_BAT	BIT(2)
#define BQ27000_FLAG_CHGS		BIT(8)
#define BQ27500_FLAG_FC			BIT(9)
#define BQ27500_FLAG_OTD		BIT(14)
#define BQ27500_FLAG_OTC		BIT(15)

#define BQ27510_SPEED 			80 * 1000

#define VOLTAGE_MAX         4300
#define VOLTAGE_MIN         3400
#define TEMP_K              2731
#define TEMP_DANGER_H       3331   // 60 celsus
#define TEMP_DANGER_L       2531   // -20 celsus
#define FULL_CHARGE         0x03
#define CHARGING            0x01
#define DISCHARGING         0x00
#define LOW_POWER_CAPACITY  0
#define LOW_POWER_VOLTAGE   3000

int  virtual_battery_enable = 0;
int g_propval_status = POWER_SUPPLY_STATUS_DISCHARGING;
int wake_up_en = 0;
extern int dwc_vbus_status(void);
static void bq27541_set(void);
extern int rk30_get_charger_mode(void);
extern int start_charge_logo_display(void* callback);


#if 0
#define DBG(x...) printk(KERN_INFO x)
#else
#define DBG(x...) do { } while (0)
#endif

/* If the system has several batteries we need a different name for each
 * of them...
 */
static DEFINE_MUTEX(battery_mutex);

struct bq27541_device_info {
	struct device 		*dev;
	struct power_supply	bat;
	struct power_supply	ac;
	struct delayed_work work;
	struct delayed_work wakeup_work;
	struct i2c_client	*client;
	unsigned int interval;
	unsigned int dc_check_pin;
	unsigned int usb_check_pin;
	unsigned int bat_num;
	int power_down;
    short int current_now;
    unsigned short int voltage;
    unsigned short int temperature;
    unsigned short int capacity;
    int charger_status;
    int health_status;
    int power_now;  //ʣ��ĵ���ֵ����λmah
  #ifdef CONFIG_CHARGING_WAKE_LOCK
    struct wake_lock wakelock;
    int wake_lock_flag;
  #endif
    struct notifier_block battery_nb;
};

static struct bq27541_device_info *bq27541_di;
static enum power_supply_property bq27541_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_POWER_NOW,
	//POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	//POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	//POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
};
#if 0
static enum power_supply_property rk29_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static enum power_supply_property rk29_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};
#endif

static ssize_t battery_proc_write (struct file *file, const char __user *buffer, size_t count, loff_t *data)
{
	char c;
	int rc;
	printk("USER:\n");
	printk("echo x >/proc/driver/power\n");
	printk("x=1,means just print log ||x=2,means log and data ||x= other,means close log\n");

	rc = get_user(c,buffer);
	if(rc)
		return rc;

	//added by zwp,c='8' means check whether we need to download firmware to bq27xxx,return 0 means yes.
	if(c == '8'){
		printk("%s,bq27541 don't need to download firmware\n",__FUNCTION__);
		return -1;//bq27541 don't need to download firmware.
	}
	if(c == '1')
		virtual_battery_enable = 1;
	else if(c == '2')
		virtual_battery_enable = 2;
	else if(c == '3')
		virtual_battery_enable = 3;
	else if(c == '9'){
		printk("%s:%d>>bq27541 set\n",__FUNCTION__,__LINE__);
		bq27541_set();
	}
	else
		virtual_battery_enable = 0;
	printk("%s,count(%d),virtual_battery_enable(%d)\n",__FUNCTION__,(int)count,virtual_battery_enable);
	return count;
}

static const struct file_operations battery_proc_fops = {
	.owner		= THIS_MODULE,
	.write		= battery_proc_write,
};

/*
 * Common code for BQ27510 devices read
 */
static int bq27541_read(struct i2c_client *client, u8 reg, u8 buf[], unsigned len)
{
	int ret;
	ret = i2c_master_reg8_recv(client, reg, buf, len, BQ27510_SPEED);
	return ret;
}

static int bq27541_write(struct i2c_client *client, u8 reg, u8 const buf[], unsigned len)
{
	int ret;
	ret = i2c_master_reg8_send(client, reg, buf, (int)len, BQ27510_SPEED);
	return ret;
}

/*
 * Return the battery temperature in tenths of degree Celsius
 * Or < 0 if something fails.
 */

#if 0
static int bq27541_battery_temperature(struct bq27541_device_info *di)
{
	int ret;
	int temp = 0;
	u8 buf[2] ={0};

	#if defined (CONFIG_NO_BATTERY_IC)
	return 258;
	#endif

	if(virtual_battery_enable == 1)
		return 125/*258*/;
	ret = bq27541_read(di->client,BQ27x00_REG_TEMP,buf,2);
	if (ret<0) {
		dev_err(di->dev, "error reading temperature\n");
		return temp;//ret
	}
	temp = get_unaligned_le16(buf);
	temp = temp - 2731;  //K
	DBG("Enter:%s %d--temp = %d\n",__FUNCTION__,__LINE__,temp);

//	rk29_pm_power_off();
	return temp;
}

/*
 * Return the battery Voltage in milivolts
 * Or < 0 if something fails.
 */



static int bq27541_battery_voltage(struct bq27541_device_info *di)
{
	int ret;
	u8 buf[2] = {0};
	int volt = 0;

	#if defined (CONFIG_NO_BATTERY_IC)
		return 4000000;
	#endif
	if(virtual_battery_enable == 1)
		return 2000000/*4000000*/;

	ret = bq27541_read(di->client,BQ27x00_REG_VOLT,buf,2);
	if (ret<0) {
		dev_err(di->dev, "error reading voltage\n");
//		gpio_set_value(POWER_ON_PIN, GPIO_LOW);
		return 4000000;//ret;
	}
	volt = get_unaligned_le16(buf);

	//bp27510 can only measure one li-lion bat
	if(di->bat_num == 2){
		volt = volt * 1000 * 2;
	}else{
		volt = volt * 1000;
	}

	if ((volt <= 3400000)  && (volt > 0) && gpio_get_value(di->dc_check_pin)){
		printk("vol smaller then 3.4V, report to android!");
		di->power_down = 1;
	}else{
		di->power_down = 0;
	}


	DBG("Enter:%s %d--volt = %d\n",__FUNCTION__,__LINE__,volt);
	return volt;
}

/*
 * Return the battery average current
 * Note that current can be negative signed as well
 * Or 0 if something fails.
 */
static int bq27541_battery_current(struct bq27541_device_info *di)
{
	int ret;
	int curr = 0;
	u8 buf[2] = {0};

	#if defined (CONFIG_NO_BATTERY_IC)
		return 22000;
	#endif
	if(virtual_battery_enable == 1)
		return 11000/*22000*/;
	ret = bq27541_read(di->client,BQ27x00_REG_AI,buf,2);
	if (ret<0) {
		dev_err(di->dev, "error reading current\n");
		return 0;
	}

	curr = get_unaligned_le16(buf);
	DBG("curr = %x \n",curr);
	if(curr>0x8000){
		curr = 0xFFFF^(curr-1);
	}
	curr = curr * 1000;
	DBG("Enter:%s %d--curr = %d\n",__FUNCTION__,__LINE__,curr);
	return curr;
}

/*
 * Return the battery Relative State-of-Charge
 * Or < 0 if something fails.
 */
static int bq27541_battery_rsoc(struct bq27541_device_info *di)
{
	int ret;
	int rsoc = 0;
	#if 0
	int nvcap = 0,facap = 0,remcap=0,fccap=0,full=0,cnt=0;
	int art = 0, artte = 0, ai = 0, tte = 0, ttf = 0, si = 0;
	int stte = 0, mli = 0, mltte = 0, ae = 0, ap = 0, ttecp = 0, cc = 0;
	#endif
	u8 buf[2];

	#if defined (CONFIG_NO_BATTERY_IC)
		return 100;
	#endif
	if(virtual_battery_enable == 1)
		return 50/*100*/;

	ret = bq27541_read(di->client,BQ27500_REG_SOC,buf,2);
	if (ret<0) {
		dev_err(di->dev, "error reading relative State-of-Charge\n");
		return 50;//ret;
	}
	rsoc = get_unaligned_le16(buf);
	DBG("Enter:%s %d--rsoc = %d\n",__FUNCTION__,__LINE__,rsoc);

	#if defined (CONFIG_NO_BATTERY_IC)
	rsoc = 100;
	#endif
	#if 0     //other register information, for debug use
	ret = bq27541_read(di->client,0x0c,buf,2);		//NominalAvailableCapacity
	nvcap = get_unaligned_le16(buf);
	DBG("\nEnter:%s %d--nvcap = %d\n",__FUNCTION__,__LINE__,nvcap);
	ret = bq27541_read(di->client,0x0e,buf,2);		//FullAvailableCapacity
	facap = get_unaligned_le16(buf);
	DBG("Enter:%s %d--facap = %d\n",__FUNCTION__,__LINE__,facap);
	ret = bq27541_read(di->client,0x10,buf,2);		//RemainingCapacity
	remcap = get_unaligned_le16(buf);
	DBG("Enter:%s %d--remcap = %d\n",__FUNCTION__,__LINE__,remcap);
	ret = bq27541_read(di->client,0x12,buf,2);		//FullChargeCapacity
	fccap = get_unaligned_le16(buf);
	DBG("Enter:%s %d--fccap = %d\n",__FUNCTION__,__LINE__,fccap);
	ret = bq27541_read(di->client,0x3c,buf,2);		//DesignCapacity
	full = get_unaligned_le16(buf);
	DBG("Enter:%s %d--DesignCapacity = %d\n",__FUNCTION__,__LINE__,full);

	buf[0] = 0x00;						//CONTROL_STATUS
	buf[1] = 0x00;
	bq27541_write(di->client,0x00,buf,2);
	ret = bq27541_read(di->client,0x00,buf,2);
	cnt = get_unaligned_le16(buf);
	DBG("Enter:%s %d--Control status = %x\n",__FUNCTION__,__LINE__,cnt);

	ret = bq27541_read(di->client,0x02,buf,2);		//AtRate
	art = get_unaligned_le16(buf);
	DBG("Enter:%s %d--AtRate = %d\n",__FUNCTION__,__LINE__,art);
	ret = bq27541_read(di->client,0x04,buf,2);		//AtRateTimeToEmpty
	artte = get_unaligned_le16(buf);
	DBG("Enter:%s %d--AtRateTimeToEmpty = %d\n",__FUNCTION__,__LINE__,artte);
	ret = bq27541_read(di->client,0x14,buf,2);		//AverageCurrent
	ai = get_unaligned_le16(buf);
	DBG("Enter:%s %d--AverageCurrent = %d\n",__FUNCTION__,__LINE__,ai);
	ret = bq27541_read(di->client,0x16,buf,2);		//TimeToEmpty
	tte = get_unaligned_le16(buf);
	DBG("Enter:%s %d--TimeToEmpty = %d\n",__FUNCTION__,__LINE__,tte);
	ret = bq27541_read(di->client,0x18,buf,2);		//TimeToFull
	ttf = get_unaligned_le16(buf);
	DBG("Enter:%s %d--TimeToFull = %d\n",__FUNCTION__,__LINE__,ttf);
	ret = bq27541_read(di->client,0x1a,buf,2);		//StandbyCurrent
	si = get_unaligned_le16(buf);
	DBG("Enter:%s %d--StandbyCurrent = %d\n",__FUNCTION__,__LINE__,si);
	ret = bq27541_read(di->client,0x1c,buf,2);		//StandbyTimeToEmpty
	stte = get_unaligned_le16(buf);
	DBG("Enter:%s %d--StandbyTimeToEmpty = %d\n",__FUNCTION__,__LINE__,stte);
	ret = bq27541_read(di->client,0x1e,buf,2);		//MaxLoadCurrent
	mli = get_unaligned_le16(buf);
	DBG("Enter:%s %d--MaxLoadCurrent = %d\n",__FUNCTION__,__LINE__,mli);
	ret = bq27541_read(di->client,0x20,buf,2);		//MaxLoadTimeToEmpty
	mltte = get_unaligned_le16(buf);
	DBG("Enter:%s %d--MaxLoadTimeToEmpty = %d\n",__FUNCTION__,__LINE__,mltte);
	ret = bq27541_read(di->client,0x22,buf,2);		//AvailableEnergy
	ae = get_unaligned_le16(buf);
	DBG("Enter:%s %d--AvailableEnergy = %d\n",__FUNCTION__,__LINE__,ae);
	ret = bq27541_read(di->client,0x24,buf,2);		//AveragePower
	ap = get_unaligned_le16(buf);
	DBG("Enter:%s %d--AveragePower = %d\n",__FUNCTION__,__LINE__,ap);
	ret = bq27541_read(di->client,0x26,buf,2);		//TTEatConstantPower
	ttecp = get_unaligned_le16(buf);
	DBG("Enter:%s %d--TTEatConstantPower = %d\n",__FUNCTION__,__LINE__,ttecp);
	ret = bq27541_read(di->client,0x2a,buf,2);		//CycleCount
	cc = get_unaligned_le16(buf);
	DBG("Enter:%s %d--CycleCount = %d\n",__FUNCTION__,__LINE__,cc);
	#endif
	return rsoc;
}
#endif
static int bq27541_battery_status(struct bq27541_device_info *di,
				  union power_supply_propval *val)
{
	u8 buf[2] = {0};
	int flags = 0;
	int status = 0;
	int ret = 0;
    int charge_mode = 0;
    struct bq27541_platform_data *pdata;
    struct i2c_client *client = di->client;
    pdata = client->dev.platform_data;

    status = POWER_SUPPLY_STATUS_UNKNOWN;

	#if defined (CONFIG_NO_BATTERY_IC)
		status = POWER_SUPPLY_STATUS_FULL;
	    goto status_end;
	#endif

	if(virtual_battery_enable == 1)
	{
		status = POWER_SUPPLY_STATUS_UNKNOWN;
	    goto status_end;
	}
	ret = bq27541_read(di->client,BQ27x00_REG_FLAGS, buf, 2);
	if (ret < 0) {
		dev_err(di->dev, "error reading flags\n");
		goto status_end;
	}
	flags = get_unaligned_le16(buf);
	DBG("Enter:%s %d--status = %x, charger mode = %d\n",__FUNCTION__,__LINE__,flags,rk30_get_charger_mode());

    charge_mode = rk30_get_charger_mode();
    if((2 == charge_mode) || (1 == charge_mode))  //charging
    {
        if(di->capacity >= 100)
            status = POWER_SUPPLY_STATUS_FULL;
        else status = POWER_SUPPLY_STATUS_CHARGING;
    }
    else status = POWER_SUPPLY_STATUS_DISCHARGING;

    if((di->capacity <= 0) && (di->current_now < 0))  //low power and charge current is little
        status = POWER_SUPPLY_STATUS_DISCHARGING;

status_end:
  #ifdef CONFIG_CHARGING_WAKE_LOCK
    if(status == POWER_SUPPLY_STATUS_CHARGING || status == POWER_SUPPLY_STATUS_FULL)
    {
        if(di->wake_lock_flag == 0)
        {
            wake_lock(&di->wakelock);
            di->wake_lock_flag = 1;
        }
    }   else  {
        if (di->wake_lock_flag == 1)
        {
            wake_unlock(&di->wakelock);
            di->wake_lock_flag = 0;
        }
    }
  #endif
	val->intval = status;
    g_propval_status = status;
	return 0;
}

static int bq27541_health_status(struct bq27541_device_info *di,
				  union power_supply_propval *val)
{
	u8 buf[2] = {0};
	int flags = 0;
	int status;
	int ret;

	val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;

	#if defined (CONFIG_NO_BATTERY_IC)
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
	return 0;
	#endif

	if(virtual_battery_enable == 1)
	{
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		return 0;
	}
	ret = bq27541_read(di->client,BQ27x00_REG_FLAGS, buf, 2);
	if (ret < 0) {
		dev_err(di->dev, "error reading flags\n");
		return ret;
	}
	flags = get_unaligned_le16(buf);
	DBG("Enter:%s %d--status = %x\n",__FUNCTION__,__LINE__,flags);

	if ((flags & BQ27500_FLAG_OTD)||(flags & BQ27500_FLAG_OTC))
		status = POWER_SUPPLY_HEALTH_OVERHEAT;
	else
		status = POWER_SUPPLY_HEALTH_GOOD;

	val->intval = status;
	return 0;
}

#if 0
/*
 * Read a time register.
 * Return < 0 if something fails.
 */
static int bq27541_battery_time(struct bq27541_device_info *di, int reg,
				union power_supply_propval *val)
{
	u8 buf[2] = {0};
	int tval = 0;
	int ret;

	ret = bq27541_read(di->client,reg,buf,2);
	if (ret<0) {
		dev_err(di->dev, "error reading register %02x\n", reg);
		return ret;
	}
	tval = get_unaligned_le16(buf);
	DBG("Enter:%s %d--tval=%d\n",__FUNCTION__,__LINE__,tval);
	if (tval == 65535)
		return -ENODATA;

	val->intval = tval * 60;
	DBG("Enter:%s %d val->intval = %d\n",__FUNCTION__,__LINE__,val->intval);
	return 0;
}
#endif
#ifdef CONFIG_CHARGER_WARNNING_LOGO
static int bq27541_low_battery_check(struct bq27541_device_info *di)
{
    int tmp = 0;
    int mode = 0;
    int cap = bq27541_battery_rsoc(di);

    struct i2c_client *client = di->client;
    struct bq27541_platform_data *pdata = client->dev.platform_data;

    if(cap < 4)
    {
        printk("low battery  \n");

        if(gpio_get_value(pdata->usb_check_pin) == GPIO_LOW)
        {
        #ifdef CONFIG_CHARGER_SMB347
            while(((mode = rk30_get_charger_mode()) == 3) && (tmp<10))
            {
                msleep(300);
                tmp++;
                printk("low battery: unknow charger mode %d times\n",tmp);
            }
        #endif
            if(mode == 2){
                //DC charger
                printk("low battery: have a dc charger in\n");
                return 0;
            }
        }
    }
    return 0;
}
#endif

int bq27541_battery_get_temp(void)
{
    if(bq27541_di == NULL)
    {
        printk("%s, no bq27541_di \n",__func__);
        return 300;
    }    
    return (bq27541_di->temperature - TEMP_K);
}
EXPORT_SYMBOL_GPL(bq27541_battery_get_temp);


#define to_bq27541_device_info(x) container_of((x), \
				struct bq27541_device_info, bat);

static int bq27541_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	int ret = 0;

	struct bq27541_device_info *di = to_bq27541_device_info(psy);
	DBG("Enter:%s %d psp= %d\n",__FUNCTION__,__LINE__,psp);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
        bq27541_battery_status(di, val);
        di->charger_status = val->intval;		
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
        val->intval = di->voltage;
        break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = di->current_now;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = di->capacity;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = di->temperature - TEMP_K;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = di->health_status;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
        val->intval = di->power_now;
        break;
	default:
		return -EINVAL;
	}
    return ret;
}
#if 0
static int rk29_ac_get_property(struct power_supply *psy,
			enum power_supply_property psp,
			union power_supply_propval *val)
{
	int ret = 0;
	struct bq27541_device_info *di = container_of(psy, struct bq27541_device_info, ac);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		if (psy->type == POWER_SUPPLY_TYPE_MAINS){
			if(gpio_get_value(di->dc_check_pin))
				val->intval = 0;	/*discharging*/
			else
				val->intval = 1;	/*charging*/
		}
		DBG("%s:%d val->intval = %d\n",__FUNCTION__,__LINE__,val->intval);
		break;

	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int rk29_usb_get_property(struct power_supply *psy,
			enum power_supply_property psp,
			union power_supply_propval *val)
{
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:

        if((g_propval_status == POWER_SUPPLY_STATUS_CHARGING) || (g_propval_status == POWER_SUPPLY_STATUS_FULL))
            val->intval = 1;
        else val->intval = 0;
		DBG("%s:%d val->intval = %d\n",__FUNCTION__,__LINE__,val->intval);
		break;

	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}
#endif
static void bq27541_powersupply_init(struct bq27541_device_info *di)
{
	di->bat.type = POWER_SUPPLY_TYPE_BATTERY;
	di->bat.properties = bq27541_battery_props;
	di->bat.num_properties = ARRAY_SIZE(bq27541_battery_props);
	di->bat.get_property = bq27541_battery_get_property;
	di->power_down = 0;
#if 0
	di->ac.name = "ac";
	di->ac.type = POWER_SUPPLY_TYPE_MAINS;
	di->ac.properties = rk29_ac_props;
	di->ac.num_properties = ARRAY_SIZE(rk29_ac_props);
	di->ac.get_property = rk29_ac_get_property;

	di->ac.name = "usb";
	di->ac.type = POWER_SUPPLY_TYPE_USB;
	di->ac.properties = rk29_usb_props;
	di->ac.num_properties = ARRAY_SIZE(rk29_usb_props);
	di->ac.get_property = rk29_usb_get_property;
#endif
}

static void bq27541_battery_update_status(struct bq27541_device_info *di)
{
    static u8 bq27541_count = 0;
    u16 read_buffer;
    u8 i2c_buf[2];
    union power_supply_propval val;

    switch (bq27541_count)
    {
        case 0:
            if (bq27541_read(di->client, BQ27x00_REG_VOLT, i2c_buf, 2) == 2)
            {
                read_buffer = (i2c_buf[1] << 8) | i2c_buf[0];
                di->voltage = read_buffer;
            }
            break;
        case 1:
            if (bq27541_read(di->client, BQ27x00_REG_TEMP, i2c_buf, 2) == 2)
            {
                read_buffer = (i2c_buf[1] << 8) | i2c_buf[0];
                di->temperature = read_buffer;
            }
            break;
        case 2:
            if(bq27541_read(di->client, BQ27x00_REG_AI, i2c_buf, 2) == 2)
            {
                read_buffer = (i2c_buf[1] << 8) | i2c_buf[0];
                di->current_now = read_buffer ;
            }
            break;
        case 3:
            if(bq27541_read(di->client, BQ27500_REG_SOC, i2c_buf, 2) == 2)
            {
                read_buffer = (i2c_buf[1] << 8) | i2c_buf[0];
                if(read_buffer==0 && di->voltage>3400)
                    di->capacity=1;
                else di->capacity = read_buffer;
            }
            break;
        case 4:
       //     bq27541_battery_status(di, &val);
       //     di->charger_status = val.intval;
            break;
        case 5:
            bq27541_health_status(di, &val);
            di->health_status = val.intval;
            break;
        case 6:
            if(bq27541_read(di->client, BQ27x00_REG_RM, i2c_buf, 2) == 2)
            {
                read_buffer = (i2c_buf[1] << 8) | i2c_buf[0];
                di->power_now = read_buffer ;
            }
            break;
    }
    if(bq27541_count++ == 0)power_supply_changed(&di->bat);
    bq27541_count %= 7;
}

static void bq27541_battery_work(struct work_struct *work)
{
	struct bq27541_device_info *di = container_of(work, struct bq27541_device_info, work.work);
	bq27541_battery_update_status(di);
	/* reschedule for the next time */
	schedule_delayed_work(&di->work, di->interval);
}

static void bq27541_set(void)
{
	struct bq27541_device_info *di;
        int i = 0;
	u8 buf[2];

	di = bq27541_di;
        printk("enter 0x41\n");
	buf[0] = 0x41;
	buf[1] = 0x00;
	bq27541_write(di->client,0x00,buf,2);

        msleep(1500);

        printk("enter 0x21\n");
	buf[0] = 0x21;
	buf[1] = 0x00;
	bq27541_write(di->client,0x00,buf,2);

	buf[0] = 0;
	buf[1] = 0;
	bq27541_read(di->client,0x00,buf,2);

      	// printk("%s: Enter:BUF[0]= 0X%x   BUF[1] = 0X%x\n",__FUNCTION__,buf[0],buf[1]);

      	while((buf[0] & 0x04)&&(i<5))
       	{
        	printk("enter more 0x21 times i = %d\n",i);
              	mdelay(1000);
       		buf[0] = 0x21;
		buf[1] = 0x00;
		bq27541_write(di->client,0x00,buf,2);

		buf[0] = 0;
		buf[1] = 0;
		bq27541_read(di->client,0x00,buf,2);
		i++;
       	}

      	if(i>5)
	   	printk("write 0x21 error\n");
	else
		printk("bq27541 write 0x21 success\n");
}
#if 0

static irqreturn_t bq27541_bat_wakeup(int irq, void *dev_id)
{
	struct bq27541_device_info *di = (struct bq27541_device_info *)dev_id;

	//printk("!!!  bq27541 bat irq vol = %d !!!\n\n\n",gpio_get_value(RK30_PIN6_PA6));

	schedule_delayed_work(&di->wakeup_work, msecs_to_jiffies(0));
	return IRQ_HANDLED;
}

static void bq27541_battery_wake_work(struct work_struct *work)
{
//    struct bq27541_device_info *di =
//		(struct bq27541_device_info *)container_of(work, struct bq27541_device_info, wakeup_work.work);
//	union power_supply_propval val;
   // rk28_send_wakeup_key();
    //printk("%s \n",__func__);
   // bq27541_battery_status(di, &val);
}
#endif

static BLOCKING_NOTIFIER_HEAD(rk_battery_chain_head);

int register_rk_battery_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&rk_battery_chain_head, nb);
}
EXPORT_SYMBOL_GPL(register_rk_battery_notifier);

int unregister_adc_battery_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&rk_battery_chain_head, nb);
}
EXPORT_SYMBOL_GPL(unregister_adc_battery_notifier);

int rk_battery_notifier_call_chain(unsigned long val)
{
	return (blocking_notifier_call_chain(&rk_battery_chain_head, val, NULL)
			== NOTIFY_BAD) ? -EINVAL : 0;
}
EXPORT_SYMBOL_GPL(rk_battery_notifier_call_chain);

extern void kernel_power_off(void);
extern int tps65910_device_shutdown(void);

static void rk_power_check(struct bq27541_device_info *dev_info)
{
	char i2c_buf[2]={0,0};
    if(virtual_battery_enable)
    {
        return;
    }
    if(bq27541_read(dev_info->client, BQ27x00_REG_VOLT, i2c_buf, 2) == 2)
    {
        dev_info->voltage = (i2c_buf[1] << 8) | i2c_buf[0];
    }

    if(bq27541_read(dev_info->client, BQ27x00_REG_AI, i2c_buf, 2) == 2)
    {
        dev_info->current_now = (i2c_buf[1] << 8) | i2c_buf[0];
    }

    if(bq27541_read(dev_info->client, BQ27500_REG_SOC, i2c_buf, 2) == 2)
    {
        dev_info->capacity = (i2c_buf[1] << 8) | i2c_buf[0];
        if(dev_info->capacity==0 && dev_info->voltage>3400)
            dev_info->capacity=1;
    }
    
    if (bq27541_read(dev_info->client, BQ27x00_REG_TEMP, i2c_buf, 2) == 2)
    {
        dev_info->temperature = (i2c_buf[1] << 8) | i2c_buf[0];
    }

	DBG("capacity now [ %d ] , voltage new [ %d ], current now [ %d ], temperature [ %d ],\n",
            dev_info->capacity,dev_info->voltage,dev_info->current_now,dev_info->temperature);
}

//0: not low power; 1: low power but usb charging; 2: low power and no charger
int rk_lowpower_check(void)
{
    int charge_mode = rk30_get_charger_mode();

    if(bq27541_di->capacity <= 0) //low power
    {
        if(charge_mode==2)  // ac charger
            return 0;
        else if(charge_mode==1)  //usb charger
            return 1;
        else return 2;           //no charger or nonstandard charger
    }
    return 0;
}
EXPORT_SYMBOL(rk_lowpower_check);

//return 1: low power; 2: charging; 3: low temp; 4: over temp
int get_battery_status(void)
{
    int charge_mode = rk30_get_charger_mode();
    DBG("%s \n",__func__);
    if(bq27541_di == NULL)
    {
        printk("%s, no bq27541_di \n",__func__);
        return 0;
    }

    rk_power_check(bq27541_di);

    if(rk_lowpower_check()   // low power
        ||(bq27541_di->temperature > (470+TEMP_K))    //stop boot up at 47 high temperature, and chager must in over temp status;
        ||((charge_mode>=4)&&(board_boot_mode() != BOOT_MODE_REBOOT)))      //charge IC NTC over temp, but not in reboot mode
    {
        return 1;                               //show battery warnning icon 
    }
    else return 0;
}
EXPORT_SYMBOL(get_battery_status);

static void poweron_lowerpoer_handle(struct bq27541_device_info *dev_info)
{
#ifdef CONFIG_LOGO_LOWERPOWER_WARNING
	if((get_battery_status()==1) && (rk_lowpower_check()!=1)){//low power and usb charger, just show warnning, do not shutdown
		mdelay (1500);
        if(rk30_get_charger_mode()>=4)mdelay (8500);
		kernel_power_off();
	}
#endif
}
static int battery_notifier_call(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct bq27541_device_info *dev_info=
		container_of(nb, struct bq27541_device_info, battery_nb);

	switch (event) {
		case BACKLIGHT_ON:
			printk("display BACKLIGHT_ON\n");
			poweron_lowerpoer_handle(dev_info);
			break;
		default:
			return NOTIFY_OK;
		}
	return NOTIFY_OK;
}

static int bq27541_battery_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	struct bq27541_device_info *di;
	struct bq27541_platform_data *pdata;
    int retval = 0;
    u8 i2c_buf[2] = {0};
	DBG("**********[zyw]  bq27541_battery_probe**************  ");
	pdata = client->dev.platform_data;

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		dev_err(&client->dev, "failed to allocate device info data\n");
		retval = -ENOMEM;
		goto batt_failed_2;
	}
	i2c_set_clientdata(client, di);
	di->dev = &client->dev;
	di->bat.name = "bq27541-battery";
	di->client = client;
	/* 500 ms between monotor runs interval */
	di->interval = msecs_to_jiffies(500);

	di->bat_num = pdata->bat_num;
	di->dc_check_pin = pdata->dc_check_pin;
	di->usb_check_pin = pdata->usb_check_pin;

    //rk_bq27541_di->voltage = VOLTAGE_MAX;
    di->charger_status = POWER_SUPPLY_STATUS_UNKNOWN;
    di->health_status = POWER_SUPPLY_HEALTH_UNKNOWN;
    di->temperature = TEMP_K + 280;
    di->current_now = 100;
    di->capacity = 50;
    di->power_now = 50;
    di->voltage = 3800;

	if (pdata->init_dc_check_pin)
		pdata->init_dc_check_pin( );

	bq27541_powersupply_init(di);

#ifdef CONFIG_CHARGER_WARNNING_LOGO
    bq27541_low_battery_check(di);
#endif

    bq27541_di = di;

	retval = power_supply_register(&client->dev, &di->bat);
	if (retval) {
		dev_err(&client->dev, "failed to register battery\n");
		goto batt_failed_4;
	}

//	retval = power_supply_register(&client->dev, &di->ac);
//	if (retval) {
//		dev_err(&client->dev, "failed to register ac\n");
//		goto batt_failed_4;
//	}

    if(bq27541_read(di->client, BQ27x00_REG_VOLT, i2c_buf, 2) == 2)  //battery presence check
    {
    	INIT_DELAYED_WORK(&di->work, bq27541_battery_work);
    	schedule_delayed_work(&di->work, di->interval);
    	dev_info(&client->dev, "support ver. %s enabled\n", DRIVER_VERSION);
    }   else {
        virtual_battery_enable = 1;
        printk("%s, no battery \n",__func__);
    }
  #ifdef CONFIG_CHARGING_WAKE_LOCK
    wake_lock_init(&di->wakelock, WAKE_LOCK_SUSPEND,"charging");
    di->wake_lock_flag = 0;
  #endif
    rk_power_check(di);
    if(((di->voltage <= LOW_POWER_VOLTAGE)&&(rk30_get_charger_mode()!=2))  //low power, not enough to light up the screen
        ||(di->temperature < (TEMP_K-90)))   //low temperature below -10DC
        kernel_power_off();
    di->battery_nb.notifier_call = battery_notifier_call;
    register_rk_battery_notifier(&di->battery_nb);

	return 0;

batt_failed_4:
	kfree(di);
batt_failed_2:
	return retval;
}

static int bq27541_battery_remove(struct i2c_client *client)
{
	struct bq27541_device_info *di = i2c_get_clientdata(client);

	power_supply_unregister(&di->bat);
    kfree(di->bat.name);
    kfree(di);
	return 0;
}

void bq27541_battery_shutdown(struct i2c_client *client)
{
    struct bq27541_device_info *di = i2c_get_clientdata(client);
    printk("%s\n",__func__);
    cancel_delayed_work_sync(&di->work);
}

static const struct i2c_device_id bq27541_id[] = {
	{ "bq27541", 0 },
};

static struct i2c_driver bq27541_battery_driver = {
	.driver = {
		.name = "bq27541",
	},
	.probe = bq27541_battery_probe,
	.remove = bq27541_battery_remove,
	.shutdown = bq27541_battery_shutdown,
	.id_table = bq27541_id,
};

static int __init bq27541_battery_init(void)
{
	int ret;

	struct proc_dir_entry * battery_proc_entry;

	ret = i2c_add_driver(&bq27541_battery_driver);
	if (ret)
		printk(KERN_ERR "Unable to register BQ27541 driver\n");

	battery_proc_entry = proc_create("driver/power",0777,NULL,&battery_proc_fops);

	return ret;
}

//module_init(bq27541_battery_init);
subsys_initcall_sync(bq27541_battery_init);
//fs_initcall(bq27541_battery_init);
//arch_initcall(bq27541_battery_init);

static void __exit bq27541_battery_exit(void)
{
	i2c_del_driver(&bq27541_battery_driver);
}
module_exit(bq27541_battery_exit);

MODULE_AUTHOR("clb");
MODULE_DESCRIPTION("BQ27541 battery monitor driver");
MODULE_LICENSE("GPL");

/*
 *	Real Time Clock interface for Linux
 *
 *	Copyright (C) 1996 Paul Gortmaker
 *
 *	This driver allows use of the real time clock (built into
 *	nearly all computers) from user space. It exports the /dev/rtc
 *	interface supporting various ioctl() and also the /proc/rtc
 *	pseudo-file for status information.
 *
 *	The ioctls can be used to set the interrupt behaviour and
 *	generation rate from the RTC via IRQ 8. Then the /dev/rtc
 *	interface can be used to make use of these timer interrupts,
 *	be they interval or alarm based.
 *
 *	The /dev/rtc interface will block on reads until an interrupt
 *	has been received. If a RTC interrupt has already happened,
 *	it will output an unsigned long and then block. The output value
 *	contains the interrupt status in the low byte and the number of
 *	interrupts since the last read in the remaining high bytes. The
 *	/dev/rtc interface can also be used with the select(2) call.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 *	Based on other minimal char device drivers, like Alan's
 *	watchdog, Ted's random, etc. etc.
 *
 *	1.07	Paul Gortmaker.
 *	1.08	Miquel van Smoorenburg: disallow certain things on the
 *		DEC Alpha as the CMOS clock is also used for other things.
 *	1.09	Nikita Schmidt: epoch support and some Alpha cleanup.
 *
 */

#define RTC_VERSION		"1.09"

#define RTC_IRQ		8	/* Can't see this changing soon.	*/
#define RTC_IO_EXTENT	0x10	/* Only really two ports, but...	*/

/*
 *	Note that *all* calls to CMOS_READ and CMOS_WRITE are done with
 *	interrupts disabled. Due to the index-port/data-port (0x70/0x71)
 *	design of the RTC, we don't want two different things trying to
 *	get to it at once. (e.g. the periodic 11 min sync from time.c vs.
 *	this driver.)
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/malloc.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>
#include <linux/mc146818rtc.h>

#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>

/*
 *	We sponge a minor off of the misc major. No need slurping
 *	up another valuable major dev number for this.
 */

#define RTC_MINOR	135

static struct wait_queue *rtc_wait;

static struct timer_list rtc_irq_timer;

static int rtc_lseek(struct inode *inode, struct file *file, off_t offset,
			int origin);

static int rtc_read(struct inode *inode, struct file *file,
			char *buf, int count);

static int rtc_ioctl(struct inode *inode, struct file *file,
			unsigned int cmd, unsigned long arg);

static int rtc_select(struct inode *inode, struct file *file,
			int sel_type, select_table *wait);

void get_rtc_time (struct rtc_time *rtc_tm);
void get_rtc_alm_time (struct rtc_time *alm_tm);
void rtc_dropped_irq(unsigned long data);

void set_rtc_irq_bit(unsigned char bit);
void mask_rtc_irq_bit(unsigned char bit);

static inline unsigned char rtc_is_updating(void);

/*
 *	Bits in rtc_status. (7 bits of room for future expansion)
 */

#define RTC_IS_OPEN		0x01	/* means /dev/rtc is in use	*/
#define RTC_TIMER_ON		0x02	/* missed irq timer active	*/

unsigned char rtc_status = 0;		/* bitmapped status byte.	*/
unsigned long rtc_freq = 0;		/* Current periodic IRQ rate	*/
unsigned long rtc_irq_data = 0;		/* our output to the world	*/

/*
 *	If this driver ever becomes modularised, it will be really nice
 *	to make the epoch retain its value across module reload...
 */

static unsigned long epoch = 1900;	/* year corresponding to 0x00	*/

unsigned char days_in_mo[] =
		{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

/*
 *	A very tiny interrupt handler. It runs with SA_INTERRUPT set,
 *	so that there is no possibility of conflicting with the
 *	set_rtc_mmss() call that happens during some timer interrupts.
 *	(See ./arch/XXXX/kernel/time.c for the set_rtc_mmss() function.)
 *
 *	On Alpha we won't get any interrupts anyway, as they all end up
 *	in the system timer code.
 */

#ifndef __alpha__
static void rtc_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	/*
	 *	Can be an alarm interrupt, update complete interrupt,
	 *	or a periodic interrupt. We store the status in the
	 *	low byte and the number of interrupts received since
	 *	the last read in the remainder of rtc_irq_data.
	 */

	rtc_irq_data += 0x100;
	rtc_irq_data &= ~0xff;
	rtc_irq_data |= (CMOS_READ(RTC_INTR_FLAGS) & 0xF0);
	wake_up_interruptible(&rtc_wait);

	if (rtc_status & RTC_TIMER_ON) {
		del_timer(&rtc_irq_timer);
		rtc_irq_timer.expires = jiffies + HZ/rtc_freq + 2*HZ/100;
		add_timer(&rtc_irq_timer);
	}
}
#endif

/*
 *	Now all the various file operations that we export.
 *	They are all useless on Alpha...  *sigh*.
 */

static int rtc_lseek(struct inode *inode, struct file *file, off_t offset,
	int origin)
{
	return -ESPIPE;
}

static int rtc_read(struct inode *inode, struct file *file, char *buf, int count)
{
#ifdef __alpha__
	return -EIO;
#else
	struct wait_queue wait = { current, NULL };
	int retval;

	if (count < sizeof(unsigned long))
		return -EINVAL;

	retval = verify_area(VERIFY_WRITE, buf, sizeof(unsigned long));
	if (retval)
		return retval;

	add_wait_queue(&rtc_wait, &wait);

	current->state = TASK_INTERRUPTIBLE;

	while (rtc_irq_data == 0) {
		if (file->f_flags & O_NONBLOCK) {
			retval = -EAGAIN;
			break;
		}
		if (current->signal & ~current->blocked) {
			retval = -ERESTARTSYS;
			break;
		}
		schedule();
	}

	if (retval == 0) {
		unsigned long data, flags;
		save_flags(flags);
		cli();
		data = rtc_irq_data;
		rtc_irq_data = 0;
		restore_flags(flags);
		memcpy_tofs(buf, &data, sizeof(unsigned long));
		retval = sizeof(unsigned long);
	}

	current->state = TASK_RUNNING;
	remove_wait_queue(&rtc_wait, &wait);

	return retval;
#endif
}

static int rtc_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
	unsigned long arg)
{

	unsigned long flags;

	switch (cmd) {
#ifndef __alpha__
		case RTC_AIE_OFF:	/* Mask alarm int. enab. bit	*/
		{
			mask_rtc_irq_bit(RTC_AIE);
			return 0;
		}
		case RTC_AIE_ON:	/* Allow alarm interrupts.	*/
		{
			set_rtc_irq_bit(RTC_AIE);
			return 0;
		}
		case RTC_PIE_OFF:	/* Mask periodic int. enab. bit	*/
		{
			mask_rtc_irq_bit(RTC_PIE);
			if (rtc_status & RTC_TIMER_ON) {
				del_timer(&rtc_irq_timer);
				rtc_status &= ~RTC_TIMER_ON;
			}
			return 0;
		}
		case RTC_PIE_ON:	/* Allow periodic ints		*/
		{

			/*
			 * We don't really want Joe User enabling more
			 * than 64Hz of interrupts on a multi-user machine.
			 */
			if ((rtc_freq > 64) && (!suser()))
				return -EACCES;

			if (!(rtc_status & RTC_TIMER_ON)) {
				rtc_status |= RTC_TIMER_ON;
				rtc_irq_timer.expires = jiffies + HZ/rtc_freq + 2*HZ/100;
				add_timer(&rtc_irq_timer);
			}
			set_rtc_irq_bit(RTC_PIE);
			return 0;
		}
		case RTC_UIE_OFF:	/* Mask ints from RTC updates.	*/
		{
			mask_rtc_irq_bit(RTC_UIE);
			return 0;
		}
		case RTC_UIE_ON:	/* Allow ints for RTC updates.	*/
		{
			set_rtc_irq_bit(RTC_UIE);
			return 0;
		}
#endif
		case RTC_ALM_READ:	/* Read the present alarm time */
		{
			/*
			 * This returns a struct rtc_time. Reading >= 0xc0
			 * means "don't care" or "match all". Only the tm_hour,
			 * tm_min, and tm_sec values are filled in.
			 */
			int retval;
			struct rtc_time alm_tm;

			retval = verify_area(VERIFY_WRITE, (struct rtc_time*)arg, sizeof(struct rtc_time));
			if (retval != 0 )
				return retval;

			get_rtc_alm_time(&alm_tm);

			memcpy_tofs((struct rtc_time*)arg, &alm_tm, sizeof(struct rtc_time));

			return 0;
		}
		case RTC_ALM_SET:	/* Store a time into the alarm */
		{
			/*
			 * This expects a struct rtc_time. Writing 0xff means
			 * "don't care" or "match all". Only the tm_hour,
			 * tm_min and tm_sec are used.
			 */
			int retval;
			unsigned char hrs, min, sec;
			struct rtc_time alm_tm;

			retval = verify_area(VERIFY_READ, (struct rtc_time*)arg, sizeof(struct rtc_time));
			if (retval != 0 )
				return retval;

			memcpy_fromfs(&alm_tm, (struct rtc_time*)arg, sizeof(struct rtc_time));

			hrs = alm_tm.tm_hour;
			min = alm_tm.tm_min;
			sec = alm_tm.tm_sec;

			save_flags(flags);
			cli();
			if (!(CMOS_READ(RTC_CONTROL) & RTC_DM_BINARY) ||
							RTC_ALWAYS_BCD)
			{
				if (sec < 60)
					BIN_TO_BCD(sec);
				else
					sec = 0xff;

				if (min < 60)
					BIN_TO_BCD(min);
				else
					min = 0xff;

				if (hrs < 24)
					BIN_TO_BCD(hrs);
				else
					hrs = 0xff;
			}
			CMOS_WRITE(hrs, RTC_HOURS_ALARM);
			CMOS_WRITE(min, RTC_MINUTES_ALARM);
			CMOS_WRITE(sec, RTC_SECONDS_ALARM);
			restore_flags(flags);

			return 0;
		}
		case RTC_RD_TIME:	/* Read the time/date from RTC	*/
		{
			int retval;
			struct rtc_time rtc_tm;

			retval = verify_area(VERIFY_WRITE, (struct rtc_time*)arg, sizeof(struct rtc_time));
			if (retval !=0 )
				return retval;

			get_rtc_time(&rtc_tm);
			memcpy_tofs((struct rtc_time*)arg, &rtc_tm, sizeof(struct rtc_time));
			return 0;
		}
		case RTC_SET_TIME:	/* Set the RTC */
		{
			int retval;
			struct rtc_time rtc_tm;
			unsigned char mon, day, hrs, min, sec, leap_yr;
			unsigned char save_control, save_freq_select;
			unsigned int yrs;
			unsigned long flags;

			if (!suser())
				return -EACCES;

			retval = verify_area(VERIFY_READ, (struct rtc_time*)arg, sizeof(struct rtc_time));
			if (retval != 0)
				return retval;

			memcpy_fromfs(&rtc_tm, (struct rtc_time*)arg, sizeof(struct rtc_time));

			yrs = rtc_tm.tm_year + 1900;
			mon = rtc_tm.tm_mon + 1;   /* tm_mon starts at zero */
			day = rtc_tm.tm_mday;
			hrs = rtc_tm.tm_hour;
			min = rtc_tm.tm_min;
			sec = rtc_tm.tm_sec;

			if (yrs < 1970)
				return -EINVAL;

			leap_yr = ((!(yrs % 4) && (yrs % 100)) || !(yrs % 400));

			if ((mon > 12) || (day == 0))
				return -EINVAL;

			if (day > (days_in_mo[mon] + ((mon == 2) && leap_yr)))
				return -EINVAL;

			if ((hrs >= 24) || (min >= 60) || (sec >= 60))
				return -EINVAL;

			if ((yrs -= epoch) > 255)    /* They are unsigned */
				return -EINVAL;

			save_flags(flags);
			cli();
			if (!(CMOS_READ(RTC_CONTROL) & RTC_DM_BINARY) || RTC_ALWAYS_BCD) {
				if (yrs > 169) {
					restore_flags(flags);
					return -EINVAL;
				}
				if (yrs >= 100)
					yrs -= 100;

				BIN_TO_BCD(sec);
				BIN_TO_BCD(min);
				BIN_TO_BCD(hrs);
				BIN_TO_BCD(day);
				BIN_TO_BCD(mon);
				BIN_TO_BCD(yrs);
			}

			save_control = CMOS_READ(RTC_CONTROL);
			CMOS_WRITE((save_control|RTC_SET), RTC_CONTROL);
			save_freq_select = CMOS_READ(RTC_FREQ_SELECT);
			CMOS_WRITE((save_freq_select|RTC_DIV_RESET2), RTC_FREQ_SELECT);

			CMOS_WRITE(yrs, RTC_YEAR);
			CMOS_WRITE(mon, RTC_MONTH);
			CMOS_WRITE(day, RTC_DAY_OF_MONTH);
			CMOS_WRITE(hrs, RTC_HOURS);
			CMOS_WRITE(min, RTC_MINUTES);
			CMOS_WRITE(sec, RTC_SECONDS);

			CMOS_WRITE(save_control, RTC_CONTROL);
			CMOS_WRITE(save_freq_select, RTC_FREQ_SELECT);

			restore_flags(flags);
			return 0;
		}
		case RTC_IRQP_READ:	/* Read the periodic IRQ rate.	*/
		{
			int retval;

			retval = verify_area(VERIFY_WRITE, (unsigned long*)arg, sizeof(unsigned long));
			if (retval != 0)
				return retval;

			memcpy_tofs((unsigned long*)arg, &rtc_freq, sizeof(unsigned long));
			return 0;
		}
#ifndef __alpha__
		case RTC_IRQP_SET:	/* Set periodic IRQ rate.	*/
		{
			int tmp = 0;
			unsigned char val;

			/*
			 * The max we can do is 8192Hz.
			 */
			if ((arg < 2) || (arg > 8192))
				return -EINVAL;
			/*
			 * We don't really want Joe User generating more
			 * than 64Hz of interrupts on a multi-user machine.
			 */
			if ((arg > 64) && (!suser()))
				return -EACCES;

			while (arg > (1<<tmp))
				tmp++;

			/*
			 * Check that the input was really a power of 2.
			 */
			if (arg != (1<<tmp))
				return -EINVAL;

			rtc_freq = arg;

			save_flags(flags);
			cli();
			val = CMOS_READ(RTC_FREQ_SELECT) & 0xf0;
			val |= (16 - tmp);
			CMOS_WRITE(val, RTC_FREQ_SELECT);
			restore_flags(flags);
			return 0;
		}
#endif
#ifdef __alpha__
		case RTC_EPOCH_READ:	/* Read the epoch.	*/
		{
			int retval;

			retval = verify_area(VERIFY_WRITE, (unsigned long*)arg, sizeof(unsigned long));
			if (retval != 0)
				return retval;

			memcpy_tofs((unsigned long*)arg, &epoch, sizeof(unsigned long));
			return 0;
		}
		case RTC_EPOCH_SET:	/* Set the epoch.	*/
		{
			/*
			 * There were no RTC clocks before 1900.
			 */
			if (arg < 1900)
				return -EINVAL;

			if (!suser())
				return -EACCES;

			epoch = arg;
			return 0;
		}
#endif
		default:
			return -EINVAL;
	}
}

/*
 *	We enforce only one user at a time here with the open/close.
 *	Also clear the previous interrupt data on an open, and clean
 *	up things on a close.
 *	On Alpha we just open, for we don't mess with interrups anyway.
 */

static int rtc_open(struct inode *inode, struct file *file)
{
#ifndef __alpha__
	if(rtc_status & RTC_IS_OPEN)
		return -EBUSY;

	rtc_status |= RTC_IS_OPEN;
	rtc_irq_data = 0;
#endif
	return 0;
}

static void rtc_release(struct inode *inode, struct file *file)
{

	/*
	 * Turn off all interrupts once the device is no longer
	 * in use, and clear the data.
	 */

#ifndef __alpha__
	unsigned char tmp;
	unsigned long flags;

	save_flags(flags);
	cli();
	tmp = CMOS_READ(RTC_CONTROL);
	tmp &=  ~RTC_PIE;
	tmp &=  ~RTC_AIE;
	tmp &=  ~RTC_UIE;
	CMOS_WRITE(tmp, RTC_CONTROL);
	CMOS_READ(RTC_INTR_FLAGS);
	restore_flags(flags);

	if (rtc_status & RTC_TIMER_ON) {
		rtc_status &= ~RTC_TIMER_ON;
		del_timer(&rtc_irq_timer);
	}

	rtc_irq_data = 0;
	rtc_status &= ~RTC_IS_OPEN;
#endif
}

#ifndef __alpha__
static int rtc_select(struct inode *inode, struct file *file,
			int sel_type, select_table *wait)
{
	if (sel_type == SEL_IN) {
		if (rtc_irq_data != 0)
			return 1;
		select_wait(&rtc_wait, wait);
	}
	return 0;
}
#endif

/*
 *	The various file operations we support.
 */

static struct file_operations rtc_fops = {
	rtc_lseek,
	rtc_read,
	NULL,		/* No write */
	NULL,		/* No readdir */
#ifdef __alpha__
	NULL,		/* No select on Alpha */
#else
	rtc_select,
#endif
	rtc_ioctl,
	NULL,		/* No mmap */
	rtc_open,
	rtc_release
};

static struct miscdevice rtc_dev=
{
	RTC_MINOR,
	"rtc",
	&rtc_fops
};

int rtc_init(void)
{
	unsigned long flags;
#ifdef __alpha__
	unsigned int year, ctrl;
	unsigned long uip_watchdog;
	char *guess = NULL;
#endif

	printk("Real Time Clock Driver v%s\n", RTC_VERSION);
#ifndef __alpha__
	if(request_irq(RTC_IRQ, rtc_interrupt, SA_INTERRUPT, "rtc", NULL))
	{
		/* Yeah right, seeing as irq 8 doesn't even hit the bus. */
		printk("rtc: IRQ %d is not free.\n", RTC_IRQ);
		return -EIO;
	}
#endif
	misc_register(&rtc_dev);
	/* Check region? Naaah! Just snarf it up. */
	request_region(RTC_PORT(0), RTC_IO_EXTENT, "rtc");
#ifdef __alpha__
	rtc_freq = HZ;

	/* Each operating system on an Alpha uses its own epoch.
	   Let's try to guess which one we are using now. */

	uip_watchdog = jiffies;
	if (rtc_is_updating() != 0)
		while (jiffies - uip_watchdog < 2*HZ/100)
			barrier();

	save_flags(flags);
	cli();
	year = CMOS_READ(RTC_YEAR);
	ctrl = CMOS_READ(RTC_CONTROL);
	restore_flags(flags);

	if (!(ctrl & RTC_DM_BINARY) || RTC_ALWAYS_BCD)
		BCD_TO_BIN(year);	/* This should never happen... */

	if (year > 10 && year < 44) {
		epoch = 1980;
		guess = "ARC console";
	} else if (year < 96) {
		epoch = 1952;
		guess = "Digital UNIX";
	}
	if (guess)
		printk("rtc: %s epoch (%ld) detected\n", guess, epoch);
#else
	init_timer(&rtc_irq_timer);
	rtc_irq_timer.function = rtc_dropped_irq;
	rtc_wait = NULL;
	save_flags(flags);
	cli();
	/* Initialize periodic freq. to CMOS reset default, which is 1024Hz */
	CMOS_WRITE(((CMOS_READ(RTC_FREQ_SELECT) & 0xF0) | 0x06), RTC_FREQ_SELECT);
	restore_flags(flags);
	rtc_freq = 1024;
#endif
	return 0;
}

/*
 *	At IRQ rates >= 4096Hz, an interrupt may get lost altogether.
 *	(usually during an IDE disk interrupt, with IRQ unmasking off)
 *	Since the interrupt handler doesn't get called, the IRQ status
 *	byte doesn't get read, and the RTC stops generating interrupts.
 *	A timer is set, and will call this function if/when that happens.
 *	To get it out of this stalled state, we just read the status.
 *	At least a jiffy of interrupts (rtc_freq/HZ) will have been lost.
 *	(You *really* shouldn't be trying to use a non-realtime system
 *	for something that requires a steady > 1KHz signal anyways.)
 */

#ifndef __alpha__
void rtc_dropped_irq(unsigned long data)
{
	unsigned long flags;

	printk(KERN_INFO "rtc: lost some interrupts at %ldHz.\n", rtc_freq);
	del_timer(&rtc_irq_timer);
	rtc_irq_timer.expires = jiffies + HZ/rtc_freq + 2*HZ/100;
	add_timer(&rtc_irq_timer);

	save_flags(flags);
	cli();
	rtc_irq_data += ((rtc_freq/HZ)<<8);
	rtc_irq_data &= ~0xff;
	rtc_irq_data |= (CMOS_READ(RTC_INTR_FLAGS) & 0xF0);	/* restart */
	restore_flags(flags);
}
#endif

/*
 *	Info exported via "/proc/rtc".
 */

int get_rtc_status(char *buf)
{
	char *p;
	struct rtc_time tm;
	unsigned char batt, ctrl;
	unsigned long flags;

	save_flags(flags);
	cli();
	batt = CMOS_READ(RTC_VALID) & RTC_VRT;
	ctrl = CMOS_READ(RTC_CONTROL);
	restore_flags(flags);

	p = buf;

	get_rtc_time(&tm);

	/*
	 * There is no way to tell if the luser has the RTC set for local
	 * time or for Universal Standard Time (GMT). Probably local though.
	 */
	p += sprintf(p,
		"rtc_time\t: %02d:%02d:%02d\n"
		"rtc_date\t: %04d-%02d-%02d\n",
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

	get_rtc_alm_time(&tm);

	/*
	 * We implicitly assume 24hr mode here. Alarm values >= 0xc0 will
	 * match any value for that particular field. Values that are
	 * greater than a valid time, but less than 0xc0 shouldn't appear.
	 */
	p += sprintf(p, "alarm\t\t: ");
	if (tm.tm_hour <= 24)
		p += sprintf(p, "%02d:", tm.tm_hour);
	else
		p += sprintf(p, "**:");

	if (tm.tm_min <= 59)
		p += sprintf(p, "%02d:", tm.tm_min);
	else
		p += sprintf(p, "**:");

	if (tm.tm_sec <= 59)
		p += sprintf(p, "%02d\n", tm.tm_sec);
	else
		p += sprintf(p, "**\n");

	p += sprintf(p,
		"DST_enable\t: %s\n"
		"BCD\t\t: %s\n"
		"24hr\t\t: %s\n"
		"square_wave\t: %s\n"
		"alarm_IRQ\t: %s\n"
		"update_IRQ\t: %s\n"
		"periodic_IRQ\t: %s\n"
		"periodic_freq\t: %ld\n"
		"batt_status\t: %s\n",
		(ctrl & RTC_DST_EN) ? "yes" : "no",
		(ctrl & RTC_DM_BINARY) ? "no" : "yes",
		(ctrl & RTC_24H) ? "yes" : "no",
		(ctrl & RTC_SQWE) ? "yes" : "no",
		(ctrl & RTC_AIE) ? "yes" : "no",
		(ctrl & RTC_UIE) ? "yes" : "no",
		(ctrl & RTC_PIE) ? "yes" : "no",
		rtc_freq,
		batt ? "okay" : "dead");

	return  p - buf;
}

/*
 * Returns true if a clock update is in progress
 */
static inline unsigned char rtc_is_updating(void)
{
	unsigned long flags;
	unsigned char uip;

	save_flags(flags);
	cli();
	uip = (CMOS_READ(RTC_FREQ_SELECT) & RTC_UIP);
	restore_flags(flags);
	return uip;
}

void get_rtc_time(struct rtc_time *rtc_tm)
{

	unsigned long flags, uip_watchdog = jiffies;
	unsigned char ctrl;

	/*
	 * read RTC once any update in progress is done. The update
	 * can take just over 2ms. We wait 10 to 20ms. There is no need to
	 * to poll-wait (up to 1s - eeccch) for the falling edge of RTC_UIP.
	 * If you need to know *exactly* when a second has started, enable
	 * periodic update complete interrupts, (via ioctl) and then
	 * immediately read /dev/rtc which will block until you get the IRQ.
	 * Once the read clears, read the RTC time (again via ioctl). Easy.
	 */

	if (rtc_is_updating() != 0)
		while (jiffies - uip_watchdog < 2*HZ/100)
			barrier();

	/*
	 * Only the values that we read from the RTC are set. We leave
	 * tm_wday, tm_yday and tm_isdst untouched. Even though the
	 * RTC has RTC_DAY_OF_WEEK, we ignore it, as it is only updated
	 * by the RTC when initially set to a non-zero value.
	 */
	save_flags(flags);
	cli();
	rtc_tm->tm_sec = CMOS_READ(RTC_SECONDS);
	rtc_tm->tm_min = CMOS_READ(RTC_MINUTES);
	rtc_tm->tm_hour = CMOS_READ(RTC_HOURS);
	rtc_tm->tm_mday = CMOS_READ(RTC_DAY_OF_MONTH);
	rtc_tm->tm_mon = CMOS_READ(RTC_MONTH);
	rtc_tm->tm_year = CMOS_READ(RTC_YEAR);
	ctrl = CMOS_READ(RTC_CONTROL);
	restore_flags(flags);

	if (!(ctrl & RTC_DM_BINARY) || RTC_ALWAYS_BCD)
	{
		BCD_TO_BIN(rtc_tm->tm_sec);
		BCD_TO_BIN(rtc_tm->tm_min);
		BCD_TO_BIN(rtc_tm->tm_hour);
		BCD_TO_BIN(rtc_tm->tm_mday);
		BCD_TO_BIN(rtc_tm->tm_mon);
		BCD_TO_BIN(rtc_tm->tm_year);
	}

	/*
	 * Account for differences between how the RTC uses the values
	 * and how they are defined in a struct rtc_time;
	 */
	if ((rtc_tm->tm_year += epoch - 1900) <= 69)
		rtc_tm->tm_year += 100;

	rtc_tm->tm_mon--;
}

void get_rtc_alm_time(struct rtc_time *alm_tm)
{
	unsigned long flags;
	unsigned char ctrl;

	/*
	 * Only the values that we read from the RTC are set. That
	 * means only tm_hour, tm_min, and tm_sec.
	 */
	save_flags(flags);
	cli();
	alm_tm->tm_sec = CMOS_READ(RTC_SECONDS_ALARM);
	alm_tm->tm_min = CMOS_READ(RTC_MINUTES_ALARM);
	alm_tm->tm_hour = CMOS_READ(RTC_HOURS_ALARM);
	ctrl = CMOS_READ(RTC_CONTROL);
	restore_flags(flags);

	if (!(ctrl & RTC_DM_BINARY) || RTC_ALWAYS_BCD)
	{
		BCD_TO_BIN(alm_tm->tm_sec);
		BCD_TO_BIN(alm_tm->tm_min);
		BCD_TO_BIN(alm_tm->tm_hour);
	}
}

/*
 * Used to disable/enable interrupts for any one of UIE, AIE, PIE.
 * Rumour has it that if you frob the interrupt enable/disable
 * bits in RTC_CONTROL, you should read RTC_INTR_FLAGS, to
 * ensure you actually start getting interrupts. Probably for
 * compatibility with older/broken chipset RTC implementations.
 * We also clear out any old irq data after an ioctl() that
 * meddles with the interrupt enable/disable bits.
 */

#ifndef __alpha__
void mask_rtc_irq_bit(unsigned char bit)
{
	unsigned char val;
	unsigned long flags;

	save_flags(flags);
	cli();
	val = CMOS_READ(RTC_CONTROL);
	val &=  ~bit;
	CMOS_WRITE(val, RTC_CONTROL);
	CMOS_READ(RTC_INTR_FLAGS);
	restore_flags(flags);
	rtc_irq_data = 0;
}

void set_rtc_irq_bit(unsigned char bit)
{
	unsigned char val;
	unsigned long flags;

	save_flags(flags);
	cli();
	val = CMOS_READ(RTC_CONTROL);
	val |= bit;
	CMOS_WRITE(val, RTC_CONTROL);
	CMOS_READ(RTC_INTR_FLAGS);
	rtc_irq_data = 0;
	restore_flags(flags);
}
#endif

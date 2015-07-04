/*
 File: dv_spi.c
 Author: Joshua Hintze (joshh@imsar.com )
 Description: A very simple implementation for using the SPI
 port on the Davinci 644x platform. Thanks for Joshua Hintze's sharing
 Thanks goes to Sean on Davinci Mailing List
 Limitations: Currently this is written to only use a single Chip Select to read/write EEPROM
 Platform Dependencies: Davinci
 Change History:

 Date               Author       Description

 2010/08/15  Rain Peng   Porting to DM6441 for read/write EEPROM, should mknod -m 666 /dev/spi c 60 1

 */
//
///////////////////////////
// INCLUDES
//////////////////////////
#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/delay.h>              // udelay()
#include <linux/fs.h>                 // everything...
#include <asm/uaccess.h>              // copy_from/to_user
#include <linux/kfifo.h>
// Definition for SPI Base Address and the local Power or Sleep Controller LPSoC
#include <mach/spi.h>
#include <linux/gpio.h>
#include <mach/hardware.h>
#include <linux/spi/spi.h>
#include <linux/clk.h>
#include "myspi.h"


// Global pointer to the clock struct. We use this to start up the LPSoC (local power system on chip)
// so our SPI peripheral has power going to it.
static struct clk *g_clkptr = NULL;

#if 0
// We will use a 1K read buffer to store data
static unsigned char *g_readbuf = NULL;
static unsigned int g_readbufcount = 0;
#else
// We will use a 1K KFIFO buffer to store data for kernel
static struct kfifo *g_kfifo = NULL;
static spinlock_t g_spinlock = SPIN_LOCK_UNLOCKED;
// We will use a 1K read buffer to exchange data with user
static unsigned char *g_readbuf = NULL;
#endif


static enum spi_selchip g_selchip = Sel_chip0 ;
static enum spi_dataformat g_dataformat = format_16bit;
static unsigned int g_chip_mask = CHIP0_MASK;
static unsigned int g_cshold_mask = 0;

static void inline wait_untilsend(void)
{
	if(NULL == g_clkptr)
	{
      return;
	}
	while(SPIBUF_RXEMPTY_MASK & SPI_REG(SPIBUF))
	{
       cpu_relax();
	}
}

static void inline add2kfifo(const unsigned char *pdata,const unsigned int len)
{
  if(kfifo_len(g_kfifo) < MAX_BUF_SIZE)
  {
	kfifo_put(g_kfifo,pdata,len);
  }
  else
  {
	 kfifo_reset(g_kfifo);
  }
}

// This function will initialize the pinmux register
// --------------------------------
// Configure GPIO Pins for SPI
// --------------------------------
static void init_pinmux(void)
{
	// Enable the SPI pins on the GPIO
	SPI_REG(PINMUX0REG) = VALUE_PINMUX0REG;
	SPI_REG(PINMUX1REG) = VALUE_PINMUX1REG;
}

static void reset_spimaster(void)
{
	SPI_REG(SPIGCR0) = 0x00000000;
	mdelay(1); // Delay for a bit
	SPI_REG(SPIGCR0) = 0x00000001;
	SPI_REG(SPIGCR1) = 0x00000003; //SPIGCR1 CLKMOD��MASTERMOD
}

// Called when a userspace program opens the file
int dv_spi_open(struct inode *inode, struct file *filp)
{
	printk("<1>open +\n");
	// Power up the SPI hardware by requesting and enabling the clock
	g_clkptr = clk_get(NULL, "spi");
	if (NULL == g_clkptr)
	{
		printk("<l>Error could not get the clock\n");
		return -ENODEV;
	}
	else
	{
		clk_enable(g_clkptr);
	}
	printk("\ninit_pinmux\n");
	init_pinmux();
	return 0;
}

// Called when a userspace program closes the file
int dv_spi_release(struct inode *inode, struct file *filp)
{
	printk("\nclose\n");
	// Place SPI peripheral into reset
	SPI_REG(SPIGCR0) = 0;
	// Remove the SPI output on the GPIO
	SPI_REG(PINMUX1REG) &= (~0x00000100);
	// Disable the clock thus removing power from the peripheral
	if (g_clkptr)
	{
		clk_disable(g_clkptr);
	}
	g_clkptr = NULL;
	return 0;
}
// Reading from the SPI device
ssize_t dv_spi_read(struct file *filp, char __user *buf, size_t count, loff_t
	*f_pos)
{
	Uarray buf_val;
	ssize_t	status = 0;

	if((NULL == buf)||(count <= 0))
	{
       return -1;
	}
	#if 0
	len = kfifo_len(g_kfifo);
	if(count > len)
	{
      count = len ;
	}
	memset(g_readbuf,0,sizeof(g_readbuf));
	kfifo_get(g_kfifo,g_readbuf,count);
	// Transferring data to user space
	status = copy_to_user(buf, g_readbuf, count);
	if(0 == status)
	{
      return count;
	}
	else
	{
      return -1;
	}
	#else
	if(SPIBUF_RXEMPTY_MASK & SPI_REG(SPIBUF))
	{
	   buf_val.udata = SPI_REG(SPIBUF);
	   count = (g_dataformat == format_8bit)?1:2;
	   // Transferring data to user space
	   status = copy_to_user(buf, buf_val.uarray,count);
	   if(0 == status)
	   {
		 return count;
	   }
	   else
	   {
		 return -1;
	   }
	}
	else
	{
       return -1;
	}
	#endif
}

// Writing to the SPI device
ssize_t dv_spi_write(struct file *filp, const char __user *buf, size_t count,
	loff_t *f_pos)
{
	size_t index = 0;
	unsigned int datareg = 0;
	Uarray buf_val;
	size_t ret = -1;

	if((NULL == buf) || (count <= 0) || (count > MAX_BUF_SIZE))
	{
       return -EINVAL ;
	}
	buf_val.udata = 0;
	while((SPI_REG(SPIBUF) & SPIBUF_RXEMPTY_MASK) == 0)
	{
		cpu_relax();
	}
	if(format_8bit == g_dataformat)
	{
	  for(index=0;index<count;)
	  {
		 buf_val.udata = SPI_REG(SPIBUF);
		 if((buf_val.udata & SPIBUF_TXFULL_MASK) == 0)
		 {
			ret = copy_from_user(&datareg, buf + index, 1);
			if(ret == 0)
			{
			   datareg &= (0x000000FF);
			   datareg |= g_chip_mask;
			   SPI_REG(SPIDAT1) = datareg;
			   printk("\n1: datareg=%x\n",datareg);
			   wait_untilsend();
			}
		 }
		 index++;
	  }
	}
	else if(format_16bit == g_dataformat)
	{
	  for(index=0;index<count;)
	  {
		 buf_val.udata = SPI_REG(SPIBUF);
		 if((buf_val.udata & SPIBUF_TXFULL_MASK) == 0)
		 {
		   ret = copy_from_user(&datareg, buf + index, 2);
		   if(ret == 0)
		   {
			 datareg &= (0x0000FFFF);
			 datareg |= g_chip_mask;
			 SPI_REG(SPIDAT1) = datareg;
			 wait_untilsend();
			 SPI_REG(SPIDAT1) = (datareg|0x8000);
			 wait_untilsend();
			 #if 0
			 buf_val.udata = SPI_REG(SPIBUF);
			 add2kfifo(&(buf_val.uarray[0]),2);
			 #endif
		   }
		 }
		 index += 2;
	  }
	}
	return count;
}

ssize_t dv_spi_ioctl(struct inode *inode, struct file *filp,\
		enum  spi_commands cmd, const unsigned long arg)
{
	int tmp = -1;

	if((cmd < Cmd_reset) || (cmd > Cmd_Selchip) || (arg < Sel_chip0)||(arg > Sel_chip1))
	{
	  return -EINVAL ;
	}
	switch(cmd)
	{
	  case Cmd_reset:
	  {
		 reset_spimaster();
		 memset(g_readbuf,0,sizeof(g_readbuf));
		 kfifo_reset(g_kfifo);
		 tmp = 0;
	  }
	  break;
	  case Cmd_enspi:
	  {
        if(Sel_chip0 == (enum  spi_selchip)arg)
		{
		   SPI_REG(SPIPC0) = 0x00000E01;
		}
		else if(Sel_chip1 == (enum  spi_selchip)arg)
		{
		   SPI_REG(SPIPC0) = 0x00000E02;
		}
		tmp = 0;
	  }
	  break;
	  case Cmd_dataformat:
	  {
		 g_dataformat = (enum spi_dataformat)arg;
		 tmp = (format_8bit == g_dataformat)?SPIFMT_8BIT:SPIFMT_16BIT;
		 SPI_REG(SPIFMT0) = tmp;
		 SPI_REG(SPIFMT1) = tmp;
		 tmp = 0;
	  }
	  break;
	  case Cmd_Cshold:
	  {
		 g_cshold_mask = (Cs_active_hold == (enum spi_cshold)arg)?BIT(28):0;
#if 0
		 ret = gpio_request(37,NULL);
		 printk("\n gpio_is_valid,ret=%d \n",ret);
		 ret = gpio_direction_output(37,1);
		 printk("\n gpio_direction_output,ret=%d \n",ret);
		 //gpio_direction_output(37, (const unsigned int)arg);
#endif
	  }
	  break;
	  case Cmd_Selchip:
	  {
		 g_selchip = (enum spi_selchip)arg;
		 if(Sel_chip0 == g_selchip)
		 {
		   SPI_REG(SPIDAT1) = 0x00000000;
		   SPI_REG(SPIDAT1) = (0x00020000 | g_cshold_mask);
		   SPI_REG(SPIDEF) = 0x00000001;
		   g_chip_mask = (CHIP0_MASK | g_cshold_mask);
		 }
		 else if(Sel_chip1 == g_selchip)
		 {
		   SPI_REG(SPIDAT1) = 0x01000000;
		   SPI_REG(SPIDAT1) = (0x00010000 | g_cshold_mask);
		   SPI_REG(SPIDEF) = 0x00000002;
		   g_chip_mask = (CHIP1_MASK | g_cshold_mask);
		 }
		 SPI_REG(SPIGCR1) = 0x01000003;
		 tmp = 0;
	  }
	  break;
	  default:
	  {
        tmp = -EINVAL;
	  }
	  break;
	}
	return tmp;
}

// Structure that declares the usual file access functions
static struct file_operations dv_spi_fops =
{
	.owner = THIS_MODULE,
	.read = dv_spi_read,
	.write = dv_spi_write,
	.ioctl = dv_spi_ioctl,
	.open = dv_spi_open,
	.release = dv_spi_release
};

static int dv_spi_init(void)
{
	int result;

	/* Registering device */
	result = register_chrdev(MAJOR_VERSION, "spi", &dv_spi_fops);
	if (result < 0)
	{
		printk("\ndv_spi: cannot obtain major number %d\n", MAJOR_VERSION);
		return result;
	}
	// Allocate space for the read buffer
	g_readbuf = kmalloc(MAX_BUF_SIZE, GFP_KERNEL);
	if (!g_readbuf)
	{
		result = -ENOMEM;
		dv_spi_exit();
		return result;
	}
	g_kfifo = kfifo_alloc(MAX_BUF_SIZE, GFP_KERNEL,&g_spinlock);
	if (!g_kfifo)
	{
		result = -ENOMEM;
		dv_spi_exit();
		return result;
	}
	printk("\nInserting SPI module\n");
	return 0;
}

static void dv_spi_exit(void)
{
	unregister_chrdev(MAJOR_VERSION, "spi");

	/* Freeing buffer memory */
	if (NULL != g_readbuf)
	{
		kfree(g_readbuf);
		g_readbuf = NULL;
	}
	if(NULL != g_kfifo)
	{
	   kfifo_free(g_kfifo);
	   g_kfifo = NULL;
	}
	dv_spi_release(0, 0);
	printk("\nRemoving SPI module\n");
}

MODULE_LICENSE("Dual BSD/GPL");
module_init(dv_spi_init);
module_exit(dv_spi_exit);

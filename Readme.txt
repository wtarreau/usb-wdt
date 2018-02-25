

                                    CDC-232


    This is the Readme file to firmware-only CDC driver for Atmel AVR
    microcontrollers. For more information please visit
    http://www.recursion.jp/avrcdc/


SUMMARY
=======
    The CDC-232 performs the CDC (Communication Device Class) connection over
    low-speed USB. It provides the RS-232C interface through virtual COM
    port. The AVR-CDC is originally developed by Osamu Tamura.
    Akira Kitazawa has significantly contributed to improve this software. 
    Willy Tarreau then modified it to turn it into a simple watchdog instead.


SPECIFICATION
=============
    Internal RC Oscillator is calibrated at startup time on ATtiny45/85.
    When the other low speed device is connected under the same host 
    controller, the ATtiny45/85 may fail to be recognized by the downstream
    broadcast packet.

    Although the CDC protocol is supported by Windows 2000/XP/(Vista/7), Mac
    OS 9.1/X, and Linux 2.4 or 2.6.31-, low-speed bulk transfer is not allowed
    by the USB standard. Use CDC-232 at your own risk.

    The device presents itself as a CDC-ACM serial port, and uses this to
    manipulate output pin PB0 used to power-off a system when it's going
    down. A LED connected to PB1 is used as well to indicate when the power
    off pin is asserted. It can both be used remotely (power on/off) and
    locally via a timer (as a watchdog). The watchdog reset is implemented
    by turning the power off, then on after 0.5 second.

    It receives the following commands as strings from the USB port :
      - 0     : disables the watchdog
      - 1..8  : schedules a reset after 2^N-1 seconds (1..255)
      - OFF   : power off
      - ON    : power on
      - RST   : triggers a reset immediately.
      - L0/L1 : turns the LED off/on (to test communication without resetting)
      - ?     : retrieves current state on 2 bytes. First byte indicates the
                level of the pin, which is set as an input when not off, allowing
                to check a power supply's status. '0' indicates it's off (or
                forced off), '1' indicates it's on (or disconnected). The second
                byte indicates the remaining amount of seconds before reset.

    All unknown characters reset the parser, so CR, LF, spaces etc will not cause
    any trouble.

    All commands which manipulate the timer or the output (0..8, ON, OFF, RST)
    automatically disable any pending timer. The device takes a great care not
    to influence the output during boot, and doesn't automatically start. This
    way it's possible to leave it connected inside a machine and to turn it on
    by software. It may also be used as a self-reset function by writing "RST"
    to the device.

    A typical use case is to connect the output via a MOSFET to the PWROK
    pin of an ATX power supply. This way, when going down it will turn the
    motherboard's power off. If the device is powered from the motherboard,
    it will automatically stop asserting the signal, letting the board
    automatically restart. A typical connection looks like this :

      +5V
      -----+------------+
           |            | BS170
       ,---+----.     G |
       | ATtiny |   S ===== D  ____
       | 45/85  +------' '----|____|---> PWROK
       `---+----'              180R
           |
      -----+---------------------------> GND

    The resistor is here to limit the output current to less than 30 mA (the
    PWROK signal is very sensitive and designed to deliver at least 200 uA,
    but in case a power supply uses a buffer on the output, we don't want to
    fry it).

    The BS170 includes a reverse diode allowing the PWROK signal to be sampled
    by the output pin when used as an input.


USAGE
=====
    [Windows XP/2000/Vista/7]
    Download "avrcdc_inf.zip" and read the instruction carefully.
 
    [Mac OS X]
    You'll see the device /dev/cu.usbmodem***.

    [Linux]
    The device will be /dev/ttyACM*.
    Linux <2.6.31 does not accept low-speed CDC without patching the kernel.
    Replace the kernel to 2.6.31 or higher.


DEVELOPMENT
===========
    Build your circuit and write firmware (cdcmega*.hex/cdctiny*.hex) into it.
    C1:104 means 0.1uF, R3:1K5 means 1.5K ohms.

    This firmware has been developed on AVR Studio 4.18 and WinAVR 20100110.
    If you couldn't invoke the project from cdc*.aps, create new GCC project
    named "at***" under "cdc232.****-**-**/" without creating initial file. 
    Select each "default/Makefile" at "Configuration Options" menu.

    There are several options you can configure in
    "Project/Configuration Options" menu, or in Makefile

    (General)
    Device      Select MCU type.   
    Frequency   Select clock. 16.5MHz is the internal RC oscillator.
                (ATtiny45/85)
                3.3V Vcc may not be enough for the higher clock operation.

    (Custom Options) add -D*** to select options below.
    UART_INVERT Reverse the polarity of TXD and RXD to connect to RS-232C
                directly (ATtiny45/85).
                Enables software-inverters (PC0 -|>o- PB0, PC1 -|>o- PB1).
                Connect RXD to PB0 and TXD to PC1. The baudrate should be
                <=2400bps (ATmega48/88/168).

    Rebuild all the codes after modifying Makefile.

    Fuse bits
                          ext  H-L
        ATtiny2313         FF CD-FF
        ATtiny45/85        FF CE-F1
        ATtiny45/85(Xtal)  FF 6E-FF / FF 6E-F1 (PLL)
        ATmega8               8F-FF
        ATmega48/88/168    FF CE-FF

	SPIEN=0, WDTON=0, CKOPT(mega8)=0,
	Crystal: Ex.8MHz/PLL(45,461), BOD: 1.8-2.7V

    * Detach the ISP programmer before restarting the device.

    The code size of AVR-CDC is 2-3KB, and 128B RAM is required at least.


USING AVR-CDC FOR FREE
======================
    The AVR-CDC is published under an Open Source compliant license.
    See the file "License.txt" for details.

    You may use this driver in a form as it is. However, if you want to
    distribute a system with your vendor name, modify these files and recompile
    them;
        1. Vendor String in usbconfig.h
        2. COMPANY and MFGNAME strings in avrcdc.inf/lowbulk.inf 



    Osamu Tamura @ Recursion Co., Ltd.
    http://www.recursion.jp/avrcdc/
    26 June 2006
    7 April 2007
    7 July 2007
    27 January 2008
    25 August 2008
    10 April 2009
    18 July 2009
    28 February 2010


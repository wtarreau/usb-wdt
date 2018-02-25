
/* Name: main.c
 * Project: AVR USB driver for CDC interface on Low-Speed USB
 * Author: Osamu Tamura
 * Creation Date: 2006-05-12
 * Tabsize: 4
 * Copyright: (c) 2006 by Recursion Co., Ltd.
 * License: Proprietary, free under certain conditions. See Documentation.
 *
 * 2006-07-08   removed zero-sized receive block
 * 2006-07-08   adapted to higher baud rate by T.Kitazawa
 * 2018-02-24   adapted to make a software USB Watchdog (wtarreau)
 *
 *   avrdude -c buspirate -P /dev/ttyUSB0 -p attiny85 -v -U lfuse:w:0xe1:m
 *   avrdude -c buspirate -P /dev/ttyUSB0 -p attiny85 -v -U flash:w:cdctiny45.hex
 */

#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <avr/wdt.h>
#include <util/delay.h>

#include "usbdrv.h"

#define HW_CDC_BULK_OUT_SIZE    2
#define HW_CDC_BULK_IN_SIZE     8

enum {
    SEND_ENCAPSULATED_COMMAND = 0,
    GET_ENCAPSULATED_RESPONSE,
    SET_COMM_FEATURE,
    GET_COMM_FEATURE,
    CLEAR_COMM_FEATURE,
    SET_LINE_CODING = 0x20,
    GET_LINE_CODING,
    SET_CONTROL_LINE_STATE,
    SEND_BREAK
};


static PROGMEM char configDescrCDC[] = {   /* USB configuration descriptor */
    9,          /* sizeof(usbDescrConfig): length of descriptor in bytes */
    USBDESCR_CONFIG,    /* descriptor type */
    67,
    0,          /* total length of data returned (including inlined descriptors) */
    2,          /* number of interfaces in this configuration */
    1,          /* index of this configuration */
    0,          /* configuration name string index */
#if USB_CFG_IS_SELF_POWERED
    (1 << 7) | USBATTR_SELFPOWER,       /* attributes */
#else
    (1 << 7),                           /* attributes */
#endif
    USB_CFG_MAX_BUS_POWER/2,            /* max USB current in 2mA units */

    /* interface descriptor follows inline: */
    9,          /* sizeof(usbDescrInterface): length of descriptor in bytes */
    USBDESCR_INTERFACE, /* descriptor type */
    0,          /* index of this interface */
    0,          /* alternate setting for this interface */
    USB_CFG_HAVE_INTRIN_ENDPOINT,   /* endpoints excl 0: number of endpoint descriptors to follow */
    USB_CFG_INTERFACE_CLASS,
    USB_CFG_INTERFACE_SUBCLASS,
    USB_CFG_INTERFACE_PROTOCOL,
    0,          /* string index for interface */

    /* CDC Class-Specific descriptor */
    5,           /* sizeof(usbDescrCDC_HeaderFn): length of descriptor in bytes */
    0x24,        /* descriptor type */
    0,           /* header functional descriptor */
    0x10, 0x01,

    4,           /* sizeof(usbDescrCDC_AcmFn): length of descriptor in bytes    */
    0x24,        /* descriptor type */
    2,           /* abstract control management functional descriptor */
    0x02,        /* SET_LINE_CODING, GET_LINE_CODING, SET_CONTROL_LINE_STATE    */

    5,           /* sizeof(usbDescrCDC_UnionFn): length of descriptor in bytes  */
    0x24,        /* descriptor type */
    6,           /* union functional descriptor */
    0,           /* CDC_COMM_INTF_ID */
    1,           /* CDC_DATA_INTF_ID */

    5,           /* sizeof(usbDescrCDC_CallMgtFn): length of descriptor in bytes */
    0x24,        /* descriptor type */
    1,           /* call management functional descriptor */
    3,           /* allow management on data interface, handles call management by itself */
    1,           /* CDC_DATA_INTF_ID */

    /* Endpoint Descriptor */
    7,           /* sizeof(usbDescrEndpoint) */
    USBDESCR_ENDPOINT,  /* descriptor type = endpoint */
    0x80|USB_CFG_EP3_NUMBER,        /* IN endpoint number 3 */
    0x03,        /* attrib: Interrupt endpoint */
    8, 0,        /* maximum packet size */
    USB_CFG_INTR_POLL_INTERVAL,        /* in ms */

    /* Interface Descriptor  */
    9,           /* sizeof(usbDescrInterface): length of descriptor in bytes */
    USBDESCR_INTERFACE,           /* descriptor type */
    1,           /* index of this interface */
    0,           /* alternate setting for this interface */
    2,           /* endpoints excl 0: number of endpoint descriptors to follow */
    0x0A,        /* Data Interface Class Codes */
    0,
    0,           /* Data Interface Class Protocol Codes */
    0,           /* string index for interface */

    /* Endpoint Descriptor */
    7,           /* sizeof(usbDescrEndpoint) */
    USBDESCR_ENDPOINT,  /* descriptor type = endpoint */
    0x01,        /* OUT endpoint number 1 */
    0x02,        /* attrib: Bulk endpoint */
    HW_CDC_BULK_OUT_SIZE, 0,        /* maximum packet size */
    0,           /* in ms */

    /* Endpoint Descriptor */
    7,           /* sizeof(usbDescrEndpoint) */
    USBDESCR_ENDPOINT,  /* descriptor type = endpoint */
    0x81,        /* IN endpoint number 1 */
    0x02,        /* attrib: Bulk endpoint */
    HW_CDC_BULK_IN_SIZE, 0,        /* maximum packet size */
    0,           /* in ms */
};


uchar usbFunctionDescriptor(usbRequest_t *rq)
{

    if(rq->wValue.bytes[1] == USBDESCR_DEVICE){
        usbMsgPtr = (uchar *)usbDescriptorDevice;
        return usbDescriptorDevice[0];
    }else{  /* must be config descriptor */
        usbMsgPtr = (uchar *)configDescrCDC;
        return sizeof(configDescrCDC);
    }
}


uchar               sendEmptyFrame;
static uchar        intr3Status;    /* used to control interrupt endpoint transmissions */
static uchar        rspbuf[HW_CDC_BULK_IN_SIZE];
static uchar        iwptr; // #bytes to send from rspbuf (8 max)

/* uwd_dur : timer in multiples of 8ms or around 1/128 s
 *   - if =0 : wdt stopped
 *   - if >0 : wdt decrementing, will assert reset on 0
 *   - if <0 : reset held, wdt incrementing, will release on 0.
 */
static short        uwd_dur; // multiples of 8 ms

static enum {
    CMD_ST_NONE = 0,
    CMD_ST_L,
    CMD_ST_O,
    CMD_ST_OF,
    CMD_ST_R,
    CMD_ST_RS,
} cmd_state = CMD_ST_NONE;

/* ------------------------------------------------------------------------- */
/* ----------------------- Watchdog manipulation --------------------------- */
/* ------------------------------------------------------------------------- */

static inline void led_set_state(uchar s)
{
    if (s)
        PORTB |=  (1 << PB1);
    else
        PORTB &= ~(1 << PB1);
}

/* assert reset and turn on the LED */
static inline void uwd_assert_reset()
{
    DDRB  |=  (1 << DDB0);
    PORTB &= ~(1 << PB0);
    led_set_state(1);
}

/* release reset and turn off the LED. The reset is turned back to pull-up. */
static inline void uwd_release_reset()
{
    PORTB |=  (1 << PB0);
    DDRB  &= ~(1 << DDB0);
    led_set_state(0);
}

/* watchdog duration: 0..8 for ~0..255s (in fact, 0..255*1.024s) */
static inline void uwd_set_duration(uchar d)
{
    uwd_dur = ((1U << d) - 1) << 7;
}

static inline void uwd_set_on()
{
    uwd_release_reset();
    uwd_set_duration(0);
}

static inline void uwd_set_off()
{
    uwd_assert_reset();
    uwd_set_duration(0);
}

static inline void uwd_reset()
{
    uwd_assert_reset();
    uwd_dur = -64; // assert for 0.5 second
}

/* ------------------------------------------------------------------------- */
/* ----------------------------- USB interface ----------------------------- */
/* ------------------------------------------------------------------------- */

uchar usbFunctionSetup(uchar data[8])
{
usbRequest_t    *rq = (void *)data;

    if((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_CLASS){    /* class request type */

        if( rq->bRequest==GET_LINE_CODING || rq->bRequest==SET_LINE_CODING ){
            return 0xff;
        /*    GET_LINE_CODING -> usbFunctionRead()    */
        /*    SET_LINE_CODING -> usbFunctionWrite()    */
        }
        if(rq->bRequest == SET_CONTROL_LINE_STATE){
            /* Report serial state (carrier detect). On several Unix platforms,
             * tty devices can only be opened when carrier detect is set.
             */
            if( intr3Status==0 )
                intr3Status = 2;
        }

        /*  Prepare bulk-in endpoint to respond to early termination   */
        if((rq->bmRequestType & USBRQ_DIR_MASK) == USBRQ_DIR_HOST_TO_DEVICE)
            sendEmptyFrame  = 1;
    }

    return 0;
}


/*---------------------------------------------------------------------------*/
/* usbFunctionRead                                                          */
/*---------------------------------------------------------------------------*/

uchar usbFunctionRead( uchar *data, uchar len )
{

    data[0] = 0; // baud
    data[1] = 0; // baud
    data[2] = 0;
    data[3] = 0;
    data[4] = 0;
    data[5] = 0;
    data[6] = 8;

    return 7;
}


/*---------------------------------------------------------------------------*/
/* usbFunctionWrite                                                          */
/*---------------------------------------------------------------------------*/

uchar usbFunctionWrite( uchar *data, uchar len )
{
    return 1;
}

/*---------------------------------------------------------------------------*/
/* DATA communication with the host below                                    */
/*---------------------------------------------------------------------------*/

void usbFunctionWriteOut( uchar *data, uchar len )
{

    /* data received from usb */
    for( ; len; len-- ) {
        uchar c = *data++;

        switch (cmd_state) {
        case CMD_ST_NONE :
            switch (c) {
            case 'L' : cmd_state = CMD_ST_L; break;
            case 'O' : cmd_state = CMD_ST_O; break;
            case 'R' : cmd_state = CMD_ST_R; break;
            case '0'...'8' :
                uwd_set_duration(c - '0');
                uwd_release_reset();
                break;
            case '?' :
                /* returns   two bytes:
                 *  - {'0'|'1'} : output state
                 *  - [0..255]  : uwd_dur
                 */
                rspbuf[0] = '0' + !!(PINB & (1 << PB0));
                rspbuf[1] = uwd_dur >> 7;
                iwptr = 2;
                break;
            default: cmd_state = CMD_ST_NONE;
            }
            break;
        case CMD_ST_L :
            switch (c) {
            case '0'...'1' : led_set_state(c - '0'); cmd_state = CMD_ST_NONE; break;
            default: cmd_state = CMD_ST_NONE;
            }
            break;
        case CMD_ST_O :
            switch (c) {
            case 'F' : cmd_state = CMD_ST_OF; break;
            case 'N' : uwd_set_on(); cmd_state = CMD_ST_NONE; break;
            default: cmd_state = CMD_ST_NONE;
            }
            break;
        case CMD_ST_OF :
            switch (c) {
            case 'F' : uwd_set_off(); cmd_state = CMD_ST_NONE; break;
            default: cmd_state = CMD_ST_NONE;
            }
            break;
        case CMD_ST_R :
            switch (c) {
            case 'S' : cmd_state = CMD_ST_RS; break;
            default: cmd_state = CMD_ST_NONE;
            }
            break;
        case CMD_ST_RS :
            switch (c) {
            case 'T' : uwd_reset(); cmd_state = CMD_ST_NONE; break;
            default: cmd_state = CMD_ST_NONE;
            }
            break;
        }
    }
}


static void hardwareInit(void)
{

    /* activate pull-ups except on USB lines */
    USB_CFG_IOPORT   = (uchar)~((1<<USB_CFG_DMINUS_BIT)|(1<<USB_CFG_DPLUS_BIT));
    /* all pins input except USB (-> USB reset) */
#ifdef USB_CFG_PULLUP_IOPORT    /* use usbDeviceConnect()/usbDeviceDisconnect() if available */
    USBDDR    = 0;    /* we do RESET by deactivating pullup */
    usbDeviceDisconnect();
#else
    USBDDR    = (1<<USB_CFG_DMINUS_BIT)|(1<<USB_CFG_DPLUS_BIT);
#endif

    /* 250 ms disconnect */
    wdt_reset();
    _delay_ms(250);

#ifdef USB_CFG_PULLUP_IOPORT
    usbDeviceConnect();
#else
    USBDDR    = 0;      /*  remove USB reset condition */
#endif

    // PB1 as output (LED)
    DDRB |= (1 << DDB1);

    uwd_set_duration(0);
    uwd_release_reset();
}


int main(void)
{
    wdt_enable(WDTO_1S);
#if USB_CFG_HAVE_MEASURE_FRAME_LENGTH
	oscInit();
#endif
    hardwareInit();
    usbInit();

    intr3Status = 0;
    sendEmptyFrame  = 0;

    sei();
    for(;;){    /* main event loop */
        wdt_reset();
        usbPoll();

        /*
         * send to USB if needed
         */

        /*  host <= device : transmit   */
        if( usbInterruptIsReady() && (iwptr||sendEmptyFrame) ) {
            usbSetInterrupt(rspbuf, iwptr);
            sendEmptyFrame    = iwptr & HW_CDC_BULK_IN_SIZE;
            iwptr    = 0;
        }

        /*  host => device : accept     */
        if( usbAllRequestsAreDisabled() ) {
            usbEnableAllRequests();
        }

        /* We need to report rx and tx carrier after open attempt */
        if(intr3Status != 0 && usbInterruptIsReady3()){
            static uchar serialStateNotification[10] = {0xa1, 0x20, 0, 0, 0, 0, 2, 0, 3, 0};

            if(intr3Status == 2){
                usbSetInterrupt3(serialStateNotification, 8);
            }else{
                usbSetInterrupt3(serialStateNotification+8, 2);
            }
            intr3Status--;
        }

        if (uwd_dur > 0) {
            uwd_dur--;
            if (!uwd_dur)
                uwd_reset();
        }
        else if (uwd_dur < 0) {
            uwd_dur++;
            if (!uwd_dur)
                uwd_release_reset();
        }

        // Note: must not wait more than 50ms otherwise the device gets
        // disconnected! Here 8 ms is fine, as it's around 1/128 second so
        // we use it as a time base for uwd_dur.
        _delay_ms(8);
    }
    return 0;
}


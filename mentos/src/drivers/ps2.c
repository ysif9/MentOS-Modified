/// @file ps2.c
/// @brief PS/2 drivers.
/// @copyright (c) 2014-2021 This file is distributed under the MIT License.
/// See LICENSE.md for details.

// Include the kernel log levels.
#include "sys/kernel_levels.h"
/// Change the header.
#define __DEBUG_HEADER__ "[PS/2  ]"
/// Set the log level.
#define __DEBUG_LEVEL__ LOGLEVEL_DEBUG

#include "drivers/ps2.h"
#include "proc_access.h"
#include "sys/bitops.h"
#include "io/port_io.h"
#include "io/debug.h"
#include "stdbool.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

/// @brief Polling until we can receive bytes from the device.
static inline void __ps2_wait_read()
{
    int _time_out = 100000;
    while (((inportb(PS2_STATUS) & 0x01) == 0x00) && (_time_out--)) {
        pause();
    }
}

/// @brief Polling until we can send bytes to the device.
static inline void __ps2_wait_write()
{
    int _time_out = 100000;
    while (((inportb(PS2_STATUS) & 0x02) != 0x00) && (_time_out--)) {
        pause();
    }
}

void ps2_write(unsigned char data)
{
    __ps2_wait_write();
    outportb(PS2_DATA, data);
}

unsigned char ps2_read()
{
    __ps2_wait_read();
    return inportb(PS2_DATA);
}

static inline void __ps2_write_command(unsigned char command)
{
    __ps2_wait_write();
    outportb(PS2_COMMAND, command);
}

static inline unsigned char __ps2_get_controller_status()
{
    __ps2_write_command(0x20);
    return ps2_read();
}

static inline void __ps2_set_controller_status(unsigned char status)
{
    __ps2_write_command(0x60);
    ps2_write(status);
}

static inline int __ps2_is_dual_channel()
{
    return bit_check(__ps2_get_controller_status(), 6) != 0;
}

static inline void __ps2_enable_first_port()
{
    __ps2_write_command(0xAE);
}

static inline void __ps2_enable_second_port()
{
    __ps2_write_command(0xA8);
}

static inline void __ps2_disable_first_port()
{
    __ps2_write_command(0xAD);
}

static inline void __ps2_disable_second_port()
{
    __ps2_write_command(0xA7);
}

static inline void __ps2_write_first_port(unsigned char byte)
{
    ps2_write(byte);
}

static inline void __ps2_write_second_port(unsigned char byte)
{
    __ps2_write_command(0xD4);
    ps2_write(byte);
}

int ps2_initialize()
{
    unsigned char status, response;
    bool_t dual;

    status = __ps2_get_controller_status();
    pr_debug("Status   : %s (%3d | %02x)\n", dec_to_binary(status, 8), status, status);

    // ========================================================================
    // Step 1: Disable Devices
    // So that any PS/2 devices can't send data at the wrong time and mess up
    // your initialisation; start by sending a command 0xAD and command 0xA7 to
    // the PS/2 controller. If the controller is a "single channel" device, it
    // will ignore the "command 0xA7".

    pr_debug("Disabling first port...\n");
    __ps2_disable_first_port();

    pr_debug("Disabling second port...\n");
    __ps2_disable_second_port();

    // ========================================================================
    // Step 2: Flush The Output Buffer
    // Sometimes (e.g. due to interrupt controlled initialisation causing a lost
    // IRQ) data can get stuck in the PS/2 controller's output buffer. To guard
    // against this, now that the devices are disabled (and can't send more data
    // to the output buffer) it can be a good idea to flush the controller's
    // output buffer. There's 2 ways to do this - poll bit 0 of the Status
    // Register (while reading from IO Port 0x60 if/when bit 0 becomes set), or
    // read from IO Port 0x60 without testing bit 0. Either way should work (as
    // you're discarding the data and don't care what it was).

    pr_debug("Flushing the output buffer...\n");
    ps2_read();

    // ========================================================================
    // Step 3: Set the Controller Configuration Byte
    // Because some bits of the Controller Configuration Byte are "unknown",
    // this means reading the old value (command 0x20), changing some bits, then
    // writing the changed value back (command 0x60). You want to disable all
    // IRQs and disable translation (clear bits 0, 1 and 6).
    // While you've got the Configuration Byte, test if bit 5 was set. If it was
    // clear, then you know it can't be a "dual channel" PS/2 controller
    // (because the second PS/2 port should be disabled).

    // Get the status.
    status = __ps2_get_controller_status();
    pr_debug("Disable IRQs and translation...\n");
    // Clear bits 0, 1 and 6.
    bit_clear_assign(status, 0);
    bit_clear_assign(status, 1);
    // We want to keep the translation from Set 2/3 to Set 1, active.
    //bit_clear_assign(status, 6);
    __ps2_set_controller_status(status);
    pr_debug("Status   : %s (%3d | %02x)\n", dec_to_binary(status, 8), status, status);

    // ========================================================================
    // Step 4: Perform Controller Self Test
    // To test the PS/2 controller, send command 0xAA to it. Then wait for its
    // response and check it replied with 0x55. Note: this can reset the PS/2
    // controller on some hardware (tested on a 2016 laptop). At the very least,
    // the Controller Configuration Byte should be restored for compatibility
    // with such hardware. You can either determine the correct value yourself
    // based on the above table or restore the value read before issuing 0xAA.

    // Send 0xAA to the controller.
    __ps2_write_command(0xAA);
    // Read the response.
    if ((response = ps2_read()) != 0x55) {
        pr_err("Self-test failed : %d\n", response);
        return 1;
    }
    // Restore the value read before issuing 0xAA.
    __ps2_set_controller_status(status);

    // ========================================================================
    // Step 5: Determine If There Are 2 Channels
    // Enable the second PS/2 port and read the Controller Configuration Byte
    // again. Now, bit 5 of the Controller Configuration Byte should be clear -
    // if it's set then you know it can't be a "dual channel" PS/2 controller
    // (because the second PS/2 port should be enabled). If it is a dual channel
    // device, disable the second PS/2 port again.

    // Enable the second port.
    __ps2_enable_second_port();
    // Read the status.
    status = __ps2_get_controller_status();
    // Check if it is a dual channel PS/2 device.
    if ((dual = !bit_check(status, 5))) {
        pr_debug("Recognized a `dual channel` PS/2 controller...\n");
        __ps2_disable_second_port();
    } else {
        pr_debug("Recognized a `single channel` PS/2 controller...\n");
    }

    // ========================================================================
    // Step 6: Perform Interface Tests
    // This step tests the PS/2 ports. Use command 0xAB to test the first PS/2
    // port, then check the result. Then (if it's a "dual channel" controller)
    // use command 0xA9 to test the second PS/2 port, then check the result.
    // At this stage, check to see how many PS/2 ports are left. If there aren't
    // any that work you can just give up (display errors and terminate the PS/2
    // Controller driver).
    // Note: If one of the PS/2 ports on a dual PS/2 controller fails, then you
    // can still keep using/supporting the other PS/2 port.

    __ps2_write_command(0xAB);
    if ((response = ps2_read())) {
        pr_err("Interface test failed on first port : %d\n", response);
        return 1;
    }
    // If it is a dual channel, check the second port.
    if (dual) {
        __ps2_write_command(0xA9);
        if ((response = ps2_read())) {
            pr_err("Interface test failed on second port : %d\n", response);
            return 1;
        }
    }

    // ========================================================================
    // Step 7: Enable Devices
    // Enable any PS/2 port that exists and works.
    // If you're using IRQs (recommended), also enable interrupts for any
    // (usable) PS/2 ports in the Controller Configuration Byte (set bit 0 for
    // the first PS/2 port, and/or bit 1 for the second PS/2 port, then set it
    // with command 0x60).

    // Enable the first port.
    __ps2_enable_first_port();
    // Enable the second port.
    if (dual)
        __ps2_enable_second_port();
    // Get the status.
    status = __ps2_get_controller_status();
    pr_debug("Status   : %s (%3d | %02x)\n", dec_to_binary(status, 8), status, status);
    // Set bit 0 for the first port.
    bit_set_assign(status, 0);
    if (dual) {
        // Set bit 1 for the second port.
        bit_set_assign(status, 1);
    }
    __ps2_set_controller_status(status);

    // ========================================================================
    // Step 8: Reset Devices
    // All PS/2 devices should support the "reset" command (which is a command
    // for the device, and not a command for the PS/2 Controller).
    // To send the reset, just send the byte 0xFF to each (usable) device. The
    // device/s will respond with 0xFA (success) or 0xFC (failure), or won't
    // respond at all (no device present). If your code supports "hot-plug PS/2
    // devices" (see later), then you can assume each device is "not present"
    // and let the hot-plug code figure out that the device is present if/when
    // 0xFA or 0xFC is received on a PS/2 port.

    // Reset first port.
    __ps2_write_first_port(0xFF);
    // Wait for `command acknowledged`.
    if ((response = ps2_read()) != 0xFA) {
        pr_err("Failed to reset first PS/2 port: %d\n", response);
        return 1;
    }
    // Wait for `self test successful`.
    if ((response = ps2_read()) != 0xAA) {
        pr_err("Failed to reset first PS/2 port: %d\n", response);
        return 1;
    }

    // Reset second port.
    __ps2_write_second_port(0xFF);
    // Wait for `command acknowledged`.
    if ((response = ps2_read()) != 0xFA) {
        pr_err("Failed to reset first PS/2 port: %d\n", response);
        return 1;
    }
    // Wait for `self test successful`.
    if ((response = ps2_read()) != 0xAA) {
        pr_err("Failed to reset first PS/2 port: %d\n", response);
        return 1;
    }

    // Get the final status.
    status = __ps2_get_controller_status();
    pr_debug("Status   : %s (%3d | %02x)\n", dec_to_binary(status, 8), status, status);

    return 0;
}
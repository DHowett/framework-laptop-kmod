A kernel module that exposes the Framework Laptop (13, 16)'s battery charge limit and LEDs to userspace.

## Building

By default, this project will try to build a module for your running kernel.

```console
$ make
```

If you need to target a different kernel, set `KDIR`:

```console
$ make KDIR=/usr/src/linux-6.5
```

## Usage

You can install the module systemwide with `make modules_install`, or you can `insmod ./framework_laptop.ko`.

This module requires `cros_ec` and `cros_ec_lpcs` to be loaded and functional.

> **Note**
> For the Framework Laptop 13 AMD Ryzen 7040 series and the Framework Laptop 16,
> you will need to apply [this patch](https://lore.kernel.org/chrome-platform/20231005160701.19987-1-dustin@howett.net/) to your kernel sources.

### Battery Charge Limit

- Exposed via `charge_control_end_threshold`, available on `BAT1`
   - `/sys/class/power_supply/BAT1/charge_control_end_threshold`

### LEDs

- `/sys/class/leds/framework_laptop::kbd_backlight`

# q8-hardware manager

The q8-hardware manager is a special module to deal with allwinner
q8-tablet hardware differences. Since each q8-tablet batch produced
may have a different touchscreen-controller / accelerometer / wifi
ship, we cannot simply have a single devicetree file to describe
this tablets.

The standard devicetree files describe the bare minimum and this
module probes the i2c bus to find out which hardware is present and
then dynamically modifies the devicetree to match the actual hardware.

This is a special out-of-tree version of this module intended as a
stop-gap measure while we are figuring out upstream where and how
exactly to do the hardware autodetection these tablets need.

# Installation instructions

To do a native build and install run the following commands:

    make
    sudo make install

For a cross-compile do:

    make ARCH=arm CROSS_COMPILE=arm-linux-gnu- KBUILD=<path-to-arm-kbuild>

And the manually install the module on your q8 tablet and modify the
bootup scripts to load it (the module will not autoload!).

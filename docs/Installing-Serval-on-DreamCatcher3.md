
To use an Outernet Receiver as a satellite receiver node for the Serval Mesh
you MUST use a custom Armbian installation.

1. Copy the armbian image onto an SD card using dd, as for the skylark image.
Get the image from here:
https://archive.othernet.is/Dreamcatcher3%20Armbian/

NOTE: We have tested with Armbian 5.41.  Note that you MUST use the image
supplied by Outernet/Othernet, as their board doesn't run mainline
Armbian.

NOTE: This version sits silently for a long time after saying
"Starting kernel".  It does eventually boot though, and also
shows a login prompt on the LCD panel.

2.  Setup root password
connect a serial adapter to the on board header @ 115200bps
default root password is 1234. It forces changing on login.

In the lab, we set the root password to rootroot for convenience.

Then create account serval when prompted. We use serval as the password
for the lab units, for convenience.
This account will have sudo access.

3. With the unit in range of a runningg Mesh Extender, enable connection to
the mesh extender Wi-Fi

Use this command to connect to lab wifi:
# nmcli d wifi connect "ServalProject.org"
Then this command can be used to actually connect:
# nmtui

4. Install serval-dna and lbard
This distro has gcc and git already installed, so can clone serval-dna
source straight away:

$ git clone  https://github.com/servalproject/serval-dna.git 
$ cd serval-dna
$ git submodule init
$ git submodule update
$ sudo apt-get install libtool csh
$ libtoolize
$ autoreconf -f -i -I m4
$ ./configure
$ make

Then build lbard:

$ git clone https://github.com/servalproject/lbard.git
$ cd lbard
$ make

Then copy the demod binary supplied by outernet on.
This requires that you have installed the correct up-to-date version
of armbian (5.41) mentioned above!

To copy the demod binary on, it is easiest to connect to the same Wi-Fi
access point as the Dream Catcher board, so that you can use scp to copy
it on.  It is also possible to transfer it via the serial connection
if desired.  


Then run enable the tee bias voltage:

$ sudo apt update
$ sudo apt install i2c-tools
$ sudo i2cset -y 0 0x60 0x00 0x8B




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
connect a serial adapter to the on board header @ 115200bps,
and power on the board, and when prompted, set the root password.
To connect via serial, you can use something like: 
$ cu -fl /dev/ttyUSB0 -s 115200

(If cu complains about the -f option, use -l instead of -fl,
but note you might have some problems with hardware flow control.
Ubuntu 18.04 has the version of cu that supports the -f option.)

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

4. Install and setup serval-dna and lbard
This distro has gcc and git already installed, so can clone serval-dna
source straight away:

$ cd
$ git clone  https://github.com/servalproject/serval-dna.git 
$ cd serval-dna
$ git submodule init
$ git submodule update
$ sudo apt-get install libtool csh
$ libtoolize
$ autoreconf -f -i -I m4
$ ./configure
$ make
$ sudo make install
$ sudo /usr/local/sbin/servald keyring add
$ sudo /usr/local/sbin/servald start
$ sudo /usr/local/sbin/servald config set rhizome.http.enable 1
$ sudo /usr/local/sbin/servald config set api.restful.users.lbard.password lbard
$ sync


5. Then build lbard:

$ cd
$ git clone https://github.com/servalproject/lbard.git
$ cd lbard
$ make
$ sudo cp lbard /usr/local/bin
$ sync

6. Install demod binary

Then copy the demod binary supplied by outernet onto the board.
You can find this binary in blobs/demod.  It should be installed
as /usr/local/bin/demod

This requires that you have installed the correct up-to-date version
of armbian (5.41) mentioned above!

To copy the demod binary on, it is easiest to connect to the same Wi-Fi
access point as the Dream Catcher board, so that you can use scp to copy
it on.  It is also possible to transfer it via the serial connection
if desired.  


6. Then run enable the tee bias voltage:

$ sudo apt update
$ sudo apt install i2c-tools
$ sudo i2cset -y 0 0x60 0x00 0x8B

7. Then test-run lbard and demod

# Note that 166 is the beam type, which sets data rate etc, and 0.8795 is the frequency of the beam
# after subtracting the intermediate frequency of the LNB (in our case 10.75 GHz)
$ sudo /usr/sbin/i2cset -y 0 0x60 0x00 0x8B && sudo /usr/local/bin/demod 166 0.8795 0 &
$ sudo echo && /usr/local/bin/lbard 127.0.0.1:4110 lbard:lbard  `sudo servald keyring list | tail -1 | cut -f1 -d:` `sudo servald keyring list | tail -1 | cut -f2 -d:` /dev/null  outernetrx=/tmp/demod.socks.000

8. On the uplink computer, commence operations.  The command for uplink contains sensitive information, and is therefore not listed here.  Refer to the Final Report for the proof of concept project funded by the Humanitarian Innovation Fund.  The general form of the command, is, however:

$ lbard localhost:4110 lbard:lbard `sudo servald keyring list | tail -1 | cut -f1 -d:` `sudo servald keyring list | tail -1 | cut -f2 -d:`  outernet://uplinkurl

The uplink will then begin to uplink all bundles in the Rhizome database of the uplink Serval instance. Therefore it is normally recommended that this be an isolated node, with
no network interfaces defined.  Injecting new messages and alerts is accomplished by pushing them into this rhizome database.  Uplink occurs using 5 parallel "lanes", each of which
handles bundles of a distinct range of sizes.  Thus lane 1 is for small (<1KB) high-priority bundles.  To ensure low latencies, stale alerts should be removed from the rhizome
database after they are no longer relevant.  This could be done with a program that scrubs the rhizome database of such entries by checking the expiration information encoded in
the alerts themselves, once the alert format has been defined.


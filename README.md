LBARD: Low-Bandwidth Asynchronous Rhizome Demonstrator
======================================================
[Serval Project][], March 2016

Overview
--------

LBARD is a prototype content transport protocol designed for a network of two
or more Serval nodes connected by a low-bandwidth data link such as a UHF
packet radio or SMS.  LBARD efficiently synchronises [Rhizome][] content, such
as [MeshMS][] text messages, between the nodes.

[Rhizome][]'s standard synchronisation protocol was not designed for scarce
bandwidth, so it does not perform well in such low-bandwidth networks.  LBARD
improves on the standard protocol by prioritising bundles more carefully and by
using a novel “tree synchronisation” technique based on the XOR of content
hashes to rapidly prune commonly-held content from the negotiation stage.

Architecture
------------

LBARD's synchronisation protocol is intended to eventually replace the standard
[Rhizome][] synchronisation protocol, but for the time being it is implemented
as a separate daemon process upon which [Serval DNA][] does not depend.

In software architectural terms, LBARD acts as a content bridge between two or
more [Serval DNA][] daemon processes (one running on each node).  An **lbard**
daemon runs on each node, and uses the [Rhizome REST API][] to query and update
the local Rhizome content held by its local **servald** daemon.  For example,
the LBARD daemon uses the [GET /restful/rhizome/bundlelist.json][] REST request
to fetch a list of all local content, then communicates this list over the
low-bandwidth link with other LBARD daemons.

Each LBARD daemon prioritises the remote bundles that it does not already
possess, and requests the remote daemon to transmit those bundles, in order of
priority.

Download
--------

LBARD is available as a [Git][] repository from GitHub:
https://github.com/servalproject/lbard.  It can be downloaded using the [Git
clone][] command:

    $ git clone https://github.com/servalproject/lbard.git
    Cloning into 'lbard'...
    remote: Counting objects: 1752, done.
    remote: Compressing objects: 100% (32/32), done.
    remote: Total 1752 (delta 14), reused 0 (delta 0), pack-reused 1720
    Receiving objects: 100% (1752/1752), 417.68 KiB | 238.00 KiB/s, done.
    Resolving deltas: 100% (1172/1172), done.
    Checking connectivity... done.
    $ _

The LBARD [Git][] repository includes the [Serval DNA][] repository as a
[submodule][].  After cloning, download this submodule:

    $ cd lbard
    $ git submodule init
    Submodule 'jni/serval-dna' (https://github.com/servalproject/serval-dna.git) registered for path 'jni/serval-dna'
    $ git submodule update
    Cloning into 'jni/serval-dna'...
    remote: Counting objects: 20115, done.
    remote: Total 20115 (delta 0), reused 0 (delta 0), pack-reused 20115
    Receiving objects: 100% (20115/20115), 21.77 MiB | 3.22 MiB/s, done.
    Resolving deltas: 100% (12822/12822), done.
    Checking connectivity... done.
    Submodule path 'jni/serval-dna': checked out '2da3f12cfa1e4114072a9f1a94a8d477a4955f65'
    $ _

Build
-----

To build the LBARD daemon and other executables:

    $ make
    echo "#define VERSION_STRING \""`./md5 main.c rhizome.c txmessages.c rxmessages.c bundle_cache.c json.c peers.c serial.c radio.c golay.c httpclient.c progress.c rank.c bundles.c partials.c manifests.c monitor.c timesync.c httpd.c energy_experiment.c status_dump.c fec-3.0.1/ccsds_tables.c fec-3.0.1/encode_rs_8.c fec-3.0.1/init_rs_char.c fec-3.0.1/decode_rs_8.c bundle_tree.c sha1.c sync.c`"\"" >version.h
    clang -g -std=gnu99 -Wall -fno-omit-frame-pointer -D_GNU_SOURCE=1 -o lbard main.c rhizome.c txmessages.c rxmessages.c bundle_cache.c json.c peers.c serial.c radio.c golay.c httpclient.c progress.c rank.c bundles.c partials.c manifests.c monitor.c timesync.c httpd.c energy_experiment.c status_dump.c fec-3.0.1/ccsds_tables.c fec-3.0.1/encode_rs_8.c fec-3.0.1/init_rs_char.c fec-3.0.1/decode_rs_8.c bundle_tree.c sha1.c sync.c
    clang -g -std=gnu99 -Wall -fno-omit-frame-pointer -D_GNU_SOURCE=1 -DTEST -o manifesttest manifests.c
    clang -g -std=gnu99 -Wall -fno-omit-frame-pointer -D_GNU_SOURCE=1 -o fakecsmaradio fakecsmaradio.c
    $ _

To build the [Serval DNA][] daemon, see the [Serval DNA INSTALL instructions][].

Configuration
-------------

The [Serval DNA][] daemon must have Rhizome operations enabled:

    $ ./serval-dna/servald config set rhizome.http.enable 1
    $ _

The [REST API][] of the [Serval DNA][] daemon must be set up an authentication
username and password for the LBARD daemon; for example, to set up the username
“lbard” with the password “secretsquirrel”:

    $ ./serval-dna/servald config set api.restful.users.lbard.password secretsquirrelp
    $ _

These same credentials must then be provided to the LBARD daemon as its second
command-line argument, so that it can access [Serval DNA][]'s REST API:

    $ ./lbard 127.0.0.1:$PORT lbard:secretsquirrel $SID $RADIO pull

Testing
-------

TBC

-----
**Copyright 2016 Serval Project Inc.**  
![CC-BY-4.0](./cc-by-4.0.png)
This document is available under the [Creative Commons Attribution 4.0 International licence][CC BY 4.0].


[Serval Project]: http://www.servalproject.org/
[Rhizome]: http://developer.servalproject.org/dokuwiki/doku.php?id=content:tech:rhizome
[MeshMS]: http://developer.servalproject.org/dokuwiki/doku.php?id=content:tech:meshms
[Serval DNA]: https://github.com/servalproject/serval-dna
[Serval DNA INSTALL instructions]: https://github.com/servalproject/serval-dna/blob/development/INSTALL.md
[REST API]: https://github.com/servalproject/serval-dna/blob/development/doc/REST-API.md
[Rhizome REST API]: https://github.com/servalproject/serval-dna/blob/development/doc/REST-API-Rhizome.md
[GET /restful/rhizome/bundlelist.json]: https://github.com/servalproject/serval-dna/blob/development/doc/REST-API-Rhizome.md#get-restfulrhizomebundlelistjson
[Git]: http://git-scm.org/
[submodule]: https://git-scm.com/docs/git-submodule
[Git clone]: https://git-scm.com/docs/git-clone
[CC BY 4.0]: ./LICENSE-DOCUMENTATION.md

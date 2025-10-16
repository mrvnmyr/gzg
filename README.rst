piewin
======

Ultra-simple X11 chooser using **XCB** + **Cairo**. It reads newline-separated
entries from **stdin**, opens a fullscreen window, splits the window into
pie-like sectors (equal angular slices radiating from the center), highlights
the hovered slice, and on left-click prints the selected entry to **stdout**
and exits with code 0. Press **Esc** to cancel (no output, exit code 1).

Quick Start
-----------

.. code-block:: bash

   meson setup build
   meson compile -C build
   printf "One\nTwo\nThree\n" | ./build/piewin

Notes
-----

- Targets X11 only (no Wayland), uses non-deprecated XCB + Cairo APIs.
- No source subdirectories; everything is a single ``main.c`` plus ``meson.build``.
- For 2 items you'll see a clean half/half split; for 4 items, quadrants.
  For general N items, the window is divided into equal-angle wedges covering
  the full screen (the wedge edges extend to the window borders).
- The text is centered within each slice and scaled to be as large as possible
  without clipping, using Cairo text extents.
- Hover highlight brightens the slice's background color.
- Minimal latency design: small binary, direct XCB, immediate rendering.

Dependencies
------------

- ``xcb``, ``xcb-keysyms``
- ``cairo`` (with XCB surface support)

On Debian/Ubuntu:

.. code-block:: bash

   sudo apt-get install build-essential meson pkg-config \
        libxcb1-dev libxcb-keysyms1-dev libcairo2-dev

License
-------

Public domain / CC0. Do whatever you want.

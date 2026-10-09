#!/usr/bin/env python3
# Forwarder Manager - turn the launcher payload into a C++ array for the forwarder.
# SPDX-License-Identifier: GPL-3.0-or-later
import sys
data = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as out:
    out.write("// Generated from %s by forwarder/embed_payload.py. Do not edit.\n" % sys.argv[1])
    out.write("static const unsigned char kLauncherPayload[%d] = {\n" % len(data))
    for i in range(0, len(data), 16):
        out.write("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",\n")
    out.write("};\n")

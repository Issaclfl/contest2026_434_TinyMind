#!/usr/bin/env python3
"""Fix the ESP-IDF 5.5.5 gate that keeps the esp_netif PPP glue out of the build.

The problem
-----------
`components/esp_netif/CMakeLists.txt` decides whether to compile the PPP glue
like this:

    if(CONFIG_PPP_SUPPORT)
        list(APPEND srcs_lwip lwip/esp_netif_lwip_ppp.c lwip/netif/ppp.c)
    endif()

`esp_netif_lwip_ppp.c` is where `esp_netif_ppp_set_params()`,
`esp_netif_start_ppp()` and friends live, so when that `if()` is false the
whole server/client layer is missing and the link fails with undefined
references.

`CONFIG_PPP_SUPPORT` does exist for C code -- but only as a compatibility
alias that ESP-IDF writes into the generated sdkconfig.h:

    #define CONFIG_PPP_SUPPORT CONFIG_LWIP_PPP_SUPPORT

CMake never sees that alias: `if()` reads the `CONFIG_*` variables generated
from sdkconfig, where the real symbol is `CONFIG_LWIP_PPP_SUPPORT`.  So the
PPP glue is dead code in a stock 5.5.5 build, which is consistent with the
PPP example having been dropped from the tree.

Declaring a `PPP_SUPPORT` Kconfig symbol of our own does NOT work: it makes
confgen emit `#define CONFIG_PPP_SUPPORT 1` *in addition to* the alias above,
and the duplicate definition trips -Werror.

The fix
-------
Point the gate at the symbol that actually exists.  One line, and it is what
the condition was clearly meant to say.  Same shape of defect as the missing
ARCH_HAVE_SERIAL_TERMIOS on the SF32LB52 (board/sf32lb52_net/patches/0003):
the code was complete, the switch that enables it was wrong.

Idempotent: safe to run before every build.
"""

import os
import sys

REL = os.path.join("components", "esp_netif", "CMakeLists.txt")
OLD = "if(CONFIG_PPP_SUPPORT)"
NEW = "if(CONFIG_LWIP_PPP_SUPPORT)"


def main():
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        sys.stderr.write("IDF_PATH is not set; run this from the IDF environment.\n")
        return 2

    target = os.path.join(idf_path, REL)
    if not os.path.isfile(target):
        sys.stderr.write(f"not found: {target}\n")
        return 2

    with open(target, encoding="utf-8") as fh:
        text = fh.read()

    if NEW in text:
        print(f"already patched: {REL}")
        return 0

    if OLD not in text:
        sys.stderr.write(
            f"{REL} contains neither {OLD!r} nor {NEW!r} -- this IDF version\n"
            "differs from the one this script was written for; check it by hand.\n"
        )
        return 1

    with open(target, "w", encoding="utf-8", newline="") as fh:
        fh.write(text.replace(OLD, NEW, 1))

    print(f"patched {REL}: {OLD} -> {NEW}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

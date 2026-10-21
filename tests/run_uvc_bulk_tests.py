"""Compile the real UVC Bulk/frame sources with native USB/FreeRTOS stubs.

Run on Linux (or in WSL) with GCC/G++, after idf.py has resolved USB headers:
    python3 tests/run_uvc_bulk_tests.py --idf-path /path/to/esp-idf
"""

import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", default=os.environ.get("IDF_PATH"))
    args = parser.parse_args()
    if not args.idf_path:
        parser.error("set IDF_PATH or pass --idf-path")

    root = Path(__file__).resolve().parents[1]
    idf = Path(args.idf_path)
    usb = root / "managed_components/espressif__usb/include"
    if not (usb / "usb/usb_host.h").is_file():
        parser.error("run idf.py reconfigure first to resolve the managed USB component")
    if not (idf / "components/esp_common/include/esp_err.h").is_file():
        parser.error("--idf-path must point to an ESP-IDF checkout")

    out = root / "build/tests/uvc_bulk"
    out.mkdir(parents=True, exist_ok=True)
    stubs = root / "tests/uvc_host_stubs"
    includes = [stubs, idf / "components/esp_common/include", usb,
                root / "components/usb_host_uvc/include",
                root / "components/usb_host_uvc/include/esp_private",
                root / "components/usb_host_uvc/private_include"]
    flags = ["-Wall", "-Wextra", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", "-g", "-include", "sdkconfig.h"]
    flags += ["-I" + str(path) for path in includes]
    sources = [root / "components/usb_host_uvc/uvc_bulk.c",
               root / "components/usb_host_uvc/uvc_frame.c", stubs / "usb_host_mock.c"]
    objects = []
    for source in sources:
        obj = out / (source.stem + ".o")
        subprocess.run(["gcc", "-std=gnu17", *flags, "-c", str(source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    executable = out / "uvc_bulk_test"
    subprocess.run(["g++", "-std=gnu++20", *flags, str(root / "tests/uvc_bulk_test.cpp"),
                    *objects, "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()

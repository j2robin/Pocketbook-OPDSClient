# PocketBook OPDS Client

An OPDS catalog client built in C for PocketBook e-readers. This application allows users to connect to OPDS servers (such as COPS, Calibre-Web, and Project Gutenberg), browse catalogs, search for titles, and download books directly to the device.

## Features

* **E-Ink Optimized UI:** Built with `libinkview`. Includes visual touch feedback (screen inversion) for button presses and interactions.
* **Native Dark Mode Support:** Seamlessly integrates with PocketBook's system-wide Dark Mode (Firmware 6.8+). Book covers are intelligently processed so they retain their original colors while the rest of the interface inverts perfectly.
* **Customizable Layout:** Adjust the catalog view to display between 4 and 10 rows per page via the Server Settings menu. Fonts, thumbnails, and touch targets scale dynamically to ensure comfortable reading and tapping at any size.
* **Robust Configuration:** Settings are safely stored in a text-based file directly in the app's folder (`/mnt/ext1/applications/OPDSClient/opds_client.cfg`) to prevent system write-protection issues. The app automatically detects and migrates legacy binary save files.
* **Embedded Assets:** Icons are compiled directly into the binary, requiring no external image files for the base UI. Line-art icons smoothly adapt to Dark Mode.
* **Cache Management:** Maintains a 20MB cache limit for downloaded cover images. The app automatically deletes the oldest thumbnails when this limit is reached.
* **Search Support:** Compatible with standard OPDS search endpoints and OpenSearch. Includes URL encoding to handle multi-word searches on Python-based servers like Calibre-Web.
* **Continuous Pagination:** Tracks page numbers across server batches to provide a seamless browsing experience.
* **Network Handling:** Uses connection reuse, automatically resolves relative URLs, and implements 30-second timeouts to handle unresponsive servers without freezing the device.
* **Downloads:** Books are downloaded directly to the device's storage. 
  * *Library Integration:* Completed downloads are indexed straight away, so books appear in the native PocketBook Library without a reboot. The app broadcasts `EVT_STARTSCAN` and starts the firmware's `scanner.app`, either of which is enough on current firmware. Devices that ship no `scanner.app` fall back to the older `EVT_CONFIGCHANGED` broadcast via `iv2sh`.

---

## Installation

1. Download the compiled `OPDSClient.app` file.
2. Connect your PocketBook to your computer via USB.
3. Copy `OPDSClient.app` into the `applications` folder on your device's internal storage. *(Note: This folder may be hidden by your computer's operating system).*
4. OPTIONAL:  If you want to have a couple of starting servers aleady configured you can copy the `opds_client.cfg` file into the `applications\OPDSClient\` folder.  If you havn't run the application yet the `OPDSClient` folder won't exist. You will have to create it manually or run the app once and come back and copy this file.
5. Disconnect the device. The app will be available in your PocketBook's "Applications" menu.

---

## Navigation & Controls

The application supports both touchscreen input and hardware button navigation.

### Touchscreen Controls
* **Visual Feedback:** Tapping a book or folder will invert the row color to indicate the touch was registered while the network request processes.
* **Header Navigation:** When browsing a catalog, tap the catalog title at the top of the screen to return directly to the Server Options menu.

### Hardware Button Mappings

| Button | Context: Catalog Browsing | Context: Book Details |
| :--- | :--- | :--- |
| **Next Page** | Load the next page of the catalog. | Scroll down the book summary text. |
| **Prev Page** | Load the previous page. | Scroll up the book summary text. |
| **Menu / Home**| Opens a number pad allowing you to jump to a specific page. Supports "Time Machine" jumping back to previously loaded batches. | *No action* |

---

## Debug Logging

The app logs network requests and errors if a trigger file is present.

**To enable and access logs:**
1. Create an empty file named `LOGTRIGGER.TXT` in the app's installation folder (`/mnt/ext1/applications/OPDSClient/`).
2. Run the application and perform the actions you wish to log.
3. Open the `opds_client.log` file on your computer. This file contains `libcurl` network traces, HTTP headers, and redirect information.

I have tested on a Pocketbook ERA and Inkpad Color 3. I made the UI scalable but have not tested on any older or lower resolution devices.

---

## Compiling from Source

This application targets the PocketBook SDK using the `arm-linux-gnueabihf` toolchain. 

It is recommended to build this project using the **[5keeve PocketBook SDK 6.3.0 Docker image](https://github.com/ezdiy/docker-pocketbook-sdk)**. 

Dependencies required to compile:
* `libinkview` (PocketBook UI)
* `libcurl` (Networking)
* `libxml2` (OPDS Parsing)
* `freetype2` (Font Rendering)
* `stb_image.h` (Included header for decoding images)

To compile, run:
```bash
make
```

## Customizing Icons (Advanced/Optional)

Note: You do not need to do this to use or compile the app. The default icons are already converted and safely stored inside the `icons.h` file.

If you want to replace the default folder or book icons with your own custom images, a Python helper script is included in the source code.

1. Ensure you have Python 3 installed on your computer.
2. Replace `folder.png` and `book.png` in the project directory with your own standard, 24-bit RGB PNG files. (The app will automatically calculate the aspect ratio for the folder icon, so any standard dimensions will work).
3. Run the conversion script from your terminal:
   ```bash
   python3 image_to_c.py
   ```
    
This will instantly overwrite the `icons.h` file, converting your new images into raw C byte arrays.

Recompile the app using `make`. Your new icons are now permanently embedded in the final `.app` file!

## License

This project is licensed under the MIT License. See the LICENSE file for details.

## Acknowledgments

Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org). All rights reserved.

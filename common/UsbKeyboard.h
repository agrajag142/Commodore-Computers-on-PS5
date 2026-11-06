// UsbKeyboard.h
// A USB keyboard read straight from FreeBSD's generic USB device
// (/dev/ugenB.D), the way PS5-KBM-Bridge (coru182, GPL-3.0) does: the
// keyboard's boot-protocol interface is found in its descriptors, the PS5's
// own keyboard driver is detached from it while the app reads it, and the
// 8-byte boot reports (modifiers, 0, six key codes) arrive through USB_FS
// transfers. close() hands the keyboard back to the PS5 (USB_DEVICEENUMERATE:
// the PS5 attaches its drivers again).
//
// Header-only, shared by the C64 and the Amiga. In the PC simulator there is
// no USB: find() finds nothing.

#ifndef USB_KEYBOARD_H
#define USB_KEYBOARD_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef PS5_SIM
#include <sys/ioctl.h>
#include <sys/types.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#endif

#ifdef PS5_SIM
// the PC simulator's made-up keyboard (sim_sce.cpp, SIM_USBKEYS): 1 and a report when one comes
extern "C" int simUsbKeyboard(uint8_t report[8]);
#endif

class UsbKeyboard {
public:
  struct Info {
    char path[32];
    unsigned vid, pid;
    int iface, ep;           // the boot keyboard's interface and interrupt-IN endpoint
    int mouseIface, mouseEp; // a boot mouse on the same device (-1: none)
  };

  // The first boot-protocol keyboard among /dev/ugen0.1 - ugen2.31 (never a
  // Sony device: the DualSense). 1 found, 0 not. `seen`: how many opened.
  static int find(Info &out, int *seen = nullptr) {
    memset(&out, 0, sizeof(out));
    out.iface = out.ep = out.mouseIface = out.mouseEp = -1;
    int opened = 0, found = 0;
#ifdef PS5_SIM
    if (getenv("SIM_USBKEYS")) {
      snprintf(out.path, sizeof(out.path), "/dev/ugen1.3");
      out.vid = 0x062a, out.pid = 0x4101, out.iface = 0, out.ep = 0x81;
      opened = found = 1;
    }
#else
    for (int bus = 0; bus <= 2 && !found; bus++)
      for (int dev = 1; dev <= 31 && !found; dev++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/ugen%d.%d", bus, dev);
        int fd = ::open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        opened++;
        struct usb_device_descriptor d;
        memset(&d, 0, sizeof(d));
        static uint8_t data[2048];
        struct usb_gen_descriptor g;
        memset(&g, 0, sizeof(g));
        g.ugd_data = data;
        g.ugd_maxlen = sizeof(data);
        if (ioctl(fd, USB_GET_DEVICE_DESC, &d) == 0 && UGETW(d.idVendor) != 0x054c &&
            ioctl(fd, USB_GET_FULL_DESC, &g) == 0 && g.ugd_actlen <= sizeof(data)) {
          Info i;
          memset(&i, 0, sizeof(i));
          i.iface = i.ep = i.mouseIface = i.mouseEp = -1;
          if (bootLayout(data, g.ugd_actlen, i) && i.ep >= 0) {
            snprintf(i.path, sizeof(i.path), "%s", path);
            i.vid = UGETW(d.idVendor);
            i.pid = UGETW(d.idProduct);
            out = i;
            found = 1;
          }
        }
        ::close(fd);
      }
#endif
    if (seen) *seen = opened;
    return found;
  }

  // Takes the keyboard from the PS5 and starts reading. 0 = ok, else the
  // step that failed (1 open, 2 FS_INIT, 3 FS_OPEN, 4 FS_START); errno says why.
  int open(const Info &i) {
    close(false);
    info = i;
#ifdef PS5_SIM
    if (!getenv("SIM_USBKEYS")) {
      errno = ENODEV;
      return 1;
    }
    fd = 1;
    epOpen = true;
    return 0;
#else
    fd = ::open(i.path, O_RDWR);
    if (fd < 0) return 1;
    int q = i.iface;
    detachRc = ioctl(fd, USB_IFACE_DRIVER_DETACH, &q);
    detachErr = detachRc < 0 ? errno : 0;
    memset(fs, 0, sizeof(fs));
    struct usb_fs_init init;
    memset(&init, 0, sizeof(init));
    init.pEndpoints = fs;
    init.ep_index_max = 1;
    if (ioctl(fd, USB_FS_INIT, &init) != 0) return failed(2);
    fsInit = true;
    struct usb_fs_open op;
    memset(&op, 0, sizeof(op));
    op.ep_index = 0;
    op.ep_no = (uint8_t)i.ep;
    op.max_bufsize = 64;
    op.max_frames = 1;
    if (ioctl(fd, USB_FS_OPEN, &op) != 0) return failed(3);
    epOpen = true;
    reqLen = op.max_packet_length ? op.max_packet_length : 8; // exactly one packet (as the bridge found)
    if (reqLen > 64) reqLen = 64;
    // boot protocol (SET_PROTOCOL 0): the 8-byte reports, whatever the keyboard's own format
    struct usb_ctl_request r;
    memset(&r, 0, sizeof(r));
    r.ucr_request.bmRequestType = UT_WRITE_CLASS_INTERFACE;
    r.ucr_request.bRequest = 0x0b;
    USETW(r.ucr_request.wValue, 0);
    USETW(r.ucr_request.wIndex, (uint16_t)i.iface);
    USETW(r.ucr_request.wLength, 0);
    protocolRc = ioctl(fd, USB_DO_REQUEST, &r);
    if (arm() < 0) return failed(4);
    return 0;
#endif
  }

  // Once a frame (it never waits): 1 and the newest report when one came.
  int poll(uint8_t report[8]) {
#ifndef PS5_SIM
    if (fd < 0 || !epOpen) return 0;
    int got = 0;
    for (int guard = 0; guard < 64; guard++) {
      struct usb_fs_complete c;
      memset(&c, 0, sizeof(c));
      if (ioctl(fd, USB_FS_COMPLETE, &c) != 0) {
        if (errno == EINTR) continue;
        if (errno == ENXIO || errno == ENODEV) lost = true;
        break; // EBUSY: nothing finished
      }
      inFlight = false;
      completions++;
      if (fs[0].status == 0 && fs[0].aFrames > 0 && lens[0] > 0) {
        uint32_t n = lens[0] < 8 ? lens[0] : 8;
        memset(report, 0, 8);
        memcpy(report, buf, n);
        lastLen = lens[0];
        reports++;
        got = 1;
      } else if (fs[0].status != 0) {
        lastStatus = fs[0].status;
        if (fs[0].status == 22) clearStall = true; // USB_ERR_STALLED
      }
    }
    if (!inFlight && !lost) arm();
    return got;
#else
    if (fd < 0) return 0;
    int got = simUsbKeyboard(report);
    if (got) reports++, completions++, lastLen = 8;
    return got;
#endif
  }

  // Stops reading; giveBack: the PS5 gets the keyboard again (re-enumerated).
  void close(bool giveBack = true) {
#ifndef PS5_SIM
    if (fd < 0) return;
    if (epOpen && inFlight) {
      struct usb_fs_stop s;
      memset(&s, 0, sizeof(s));
      ioctl(fd, USB_FS_STOP, &s);
      struct usb_fs_complete c;
      for (int k = 0; k < 8; k++) {
        memset(&c, 0, sizeof(c));
        if (ioctl(fd, USB_FS_COMPLETE, &c) != 0) break;
      }
    }
    if (epOpen) {
      struct usb_fs_close c;
      memset(&c, 0, sizeof(c));
      ioctl(fd, USB_FS_CLOSE, &c);
    }
    if (fsInit) {
      struct usb_fs_uninit u;
      memset(&u, 0, sizeof(u));
      ioctl(fd, USB_FS_UNINIT, &u);
    }
    if (giveBack) {
      int dummy = 0;
      ioctl(fd, USB_DEVICEENUMERATE, &dummy);
    }
    ::close(fd);
#endif
    (void)giveBack;
    fd = -1;
    fsInit = epOpen = inFlight = lost = clearStall = false;
  }

  bool isOpen() const { return fd >= 0 && epOpen && !lost; }

  // for the test screen
  Info info{};
  int detachRc = 0, detachErr = 0, protocolRc = 0, lastStatus = 0;
  uint32_t completions = 0, reports = 0, lastLen = 0;
  bool lost = false;

  // The boot-protocol HID interfaces (class 3, subclass 1; protocol 1 keyboard,
  // 2 mouse) and their interrupt-IN endpoints, from a configuration descriptor.
  static bool bootLayout(const uint8_t *d, size_t n, Info &o) {
    int iface = -1, protocol = 0;
    for (size_t i = 0; i + 2 <= n;) {
      uint8_t len = d[i], type = d[i + 1];
      if (len < 2 || (size_t)len > n - i) return false;
      if (type == 4) {
        iface = -1;
        protocol = 0;
        if (len >= 9 && d[i + 3] == 0 && d[i + 5] == 3 && d[i + 6] == 1 && (d[i + 7] == 1 || d[i + 7] == 2)) {
          iface = d[i + 2];
          protocol = d[i + 7];
        }
      } else if (type == 5 && len >= 7 && iface >= 0 && (d[i + 2] & 0x80) && (d[i + 3] & 3) == 3) {
        if (protocol == 1 && o.ep < 0) o.iface = iface, o.ep = d[i + 2];
        if (protocol == 2 && o.mouseEp < 0) o.mouseIface = iface, o.mouseEp = d[i + 2];
      }
      i += len;
    }
    return o.ep >= 0 || o.mouseEp >= 0;
  }

private:
  int fd = -1;
  bool fsInit = false, epOpen = false, inFlight = false, clearStall = false;
  uint32_t reqLen = 8;
  uint8_t buf[64];
  void *bufs[1] = {buf};
  uint32_t lens[1] = {0};
#ifndef PS5_SIM
  struct usb_fs_endpoint fs[1];

  int failed(int step) {
    int e = errno;
    close(false);
    errno = e;
    return step;
  }
  int arm() {
    lens[0] = reqLen;
    bufs[0] = buf;
    fs[0].ppBuffer = bufs;
    fs[0].pLength = lens;
    fs[0].nFrames = 1;
    fs[0].aFrames = 0;
    fs[0].status = 0;
    fs[0].timeout = USB_FS_TIMEOUT_NONE; // an interrupt endpoint just waits for a key
    fs[0].flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK | (clearStall ? USB_FS_FLAG_CLEAR_STALL : 0);
    struct usb_fs_start s;
    memset(&s, 0, sizeof(s));
    s.ep_index = 0;
    if (ioctl(fd, USB_FS_START, &s) == 0 || errno == EBUSY) {
      inFlight = true;
      clearStall = false;
      return 1;
    }
    if (errno == ENXIO || errno == ENODEV) lost = true;
    return -1;
  }
#endif
};

#endif // USB_KEYBOARD_H

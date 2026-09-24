/*
 * One-shot, read-only AT probe for the unbound vendor interfaces of the
 * Dell DW5821e (413c:81d7). It never detaches a kernel driver, resets USB,
 * changes the device configuration, or sends a modem-setting command.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int read_value(const char *path, char *out, size_t size)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;
    int ok = fgets(out, (int)size, file) != NULL;
    fclose(file);
    if (!ok)
        return -1;
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

static int sys_value(const char *base, const char *name, char *out, size_t size)
{
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", base, name) >= (int)sizeof(path))
        return -1;
    return read_value(path, out, size);
}

static long milliseconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "2") && strcmp(argv[1], "3"))) {
        fprintf(stderr, "Usage: %s 2|3  (sends only AT\\r)\n", argv[0]);
        return 2;
    }

    int iface = atoi(argv[1]);
    int out_ep = iface == 2 ? 0x01 : 0x02;
    int in_ep = iface == 2 ? 0x82 : 0x84;
    char device[512] = {0};
    char token[80], path[512], iface_path[512];
    DIR *dir = opendir("/sys/bus/usb/devices");
    if (!dir) {
        perror("open USB sysfs");
        return 1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strchr(entry->d_name, ':'))
            continue;
        if (snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s",
                     entry->d_name) >= (int)sizeof(path))
            continue;
        if (sys_value(path, "idVendor", token, sizeof(token)) ||
            strcasecmp(token, "413c"))
            continue;
        if (sys_value(path, "idProduct", token, sizeof(token)) ||
            strcasecmp(token, "81d7"))
            continue;
        snprintf(device, sizeof(device), "%s", path);
        break;
    }
    closedir(dir);
    if (!device[0]) {
        fprintf(stderr, "DW5821e 413c:81d7 not found\n");
        return 1;
    }

    if (sys_value(device, "bConfigurationValue", token, sizeof(token)) ||
        strcmp(token, "2")) {
        fprintf(stderr, "Unexpected USB configuration; refusing to probe\n");
        return 1;
    }
    if (snprintf(iface_path, sizeof(iface_path), "%s:2.%d", device, iface) >=
        (int)sizeof(iface_path)) {
        fprintf(stderr, "USB path too long\n");
        return 1;
    }
    if (sys_value(iface_path, "bInterfaceClass", token, sizeof(token)) ||
        strcasecmp(token, "ff")) {
        fprintf(stderr, "Interface is not vendor-specific; refusing to probe\n");
        return 1;
    }
    if (snprintf(path, sizeof(path), "%s/driver", iface_path) >=
        (int)sizeof(path)) {
        fprintf(stderr, "USB path too long\n");
        return 1;
    }
    struct stat st;
    if (lstat(path, &st) == 0) {
        fprintf(stderr, "Interface already has a kernel driver; refusing to detach\n");
        return 1;
    }
    if (errno != ENOENT) {
        perror("inspect interface driver");
        return 1;
    }

    char bus[80], number[80];
    if (sys_value(device, "busnum", bus, sizeof(bus)) ||
        sys_value(device, "devnum", number, sizeof(number))) {
        fprintf(stderr, "Could not locate USB device node\n");
        return 1;
    }
    if (snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u",
                 (unsigned)atoi(bus), (unsigned)atoi(number)) >= (int)sizeof(path)) {
        fprintf(stderr, "USB device node path too long\n");
        return 1;
    }

    fprintf(stderr, "DW5821e %s interface %d, bulk out 0x%02x in 0x%02x\n",
            path, iface, out_ep, in_ep);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open USB device");
        return 1;
    }
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        perror("claim unbound interface");
        close(fd);
        return 1;
    }

    char command[] = "AT\r";
    struct usbdevfs_bulktransfer transfer = {
        .ep = (unsigned)out_ep,
        .len = sizeof(command) - 1,
        .timeout = 2000,
        .data = command,
    };
    int result = ioctl(fd, USBDEVFS_BULK, &transfer);
    if (result != (int)(sizeof(command) - 1)) {
        if (result < 0)
            perror("send AT");
        else
            fprintf(stderr, "Short AT write: %d bytes\n", result);
        ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
        close(fd);
        return 1;
    }

    long deadline = milliseconds() + 4000;
    int received = 0;
    char complete[4096] = {0};
    while (milliseconds() < deadline) {
        unsigned char response[1024];
        transfer.ep = (unsigned)in_ep;
        transfer.len = sizeof(response);
        transfer.timeout = 1000;
        transfer.data = response;
        result = ioctl(fd, USBDEVFS_BULK, &transfer);
        if (result < 0) {
            if (errno == ETIMEDOUT)
                continue;
            perror("read AT response");
            break;
        }
        for (int i = 0; i < result; i++) {
            unsigned char c = response[i];
            if (c == '\r' || c == '\n' || c == '\t' || (c >= 32 && c < 127))
                putchar(c);
            else
                putchar('?');
            if (received + i < (int)sizeof(complete) - 1)
                complete[received + i] = (char)c;
        }
        fflush(stdout);
        received += result;
        if (strstr(complete, "OK") || strstr(complete, "ERROR"))
            break;
    }
    ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
    close(fd);
    if (!received)
        fprintf(stderr, "No AT response within 4 seconds\n");
    return received ? 0 : 1;
}

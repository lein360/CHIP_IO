/*
Copyright (c) 2016 Robert Wolterman

Original BBIO Author Justin Cooper
Modified for CHIP_IO Author Robert Wolterman

This file incorporates work covered by the following copyright and
permission notice, all modified code adopts the original license:

Copyright (c) 2013 Adafruit

Original RPi.GPIO Author Ben Croston
Modified for BBIO Author Justin Cooper

This file incorporates work covered by the following copyright and
permission notice, all modified code adopts the original license:

Copyright (c) 2013 Ben Croston

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <pthread.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "event_gpio.h"
#include "common.h"

const char *stredge[4] = {"none", "rising", "falling", "both"};

// Memory Map for PUD
uint8_t *memmap;

// file descriptors
struct fdx
{
    int fd;
    int gpio;
    int initial;
    unsigned int is_evented;
    struct fdx *next;
};
struct fdx *fd_list = NULL;

// event callbacks
struct callback
{
    int fde;
    int gpio;
    int edge;
    void* data;
    void (*func)(int gpio, void* data);
    struct callback *next;
};
struct callback *callbacks = NULL;

// gpio exports
struct gpio_exp
{
    int gpio;
    struct gpio_exp *next;
};
struct gpio_exp *exported_gpios = NULL;

pthread_t threads;
dyn_int_array_t *event_occurred = NULL;
int thread_running = 0;
int epfd = -1;

// Thanks to WereCatf and Chippy-Gonzales for the Memory Mapping code/help
int map_pio_memory()
{
    if (DEBUG)
        printf(" ** map_pio_memory: opening /dev/mem **\n");
    int fd = open("/dev/mem", O_RDWR|O_SYNC);
	if(fd < 0) {
        char err[256];
        snprintf(err, sizeof(err), "map_pio_memory: could not open /dev/mem (%s)", strerror(errno));
        add_error_msg(err);
        return -1;
	}
	// uint32_t addr = 0x01c20800 & ~(getpagesize() - 1);
	//Requires memmap to be on pagesize-boundary
	if (DEBUG)
        printf(" ** map_pio_memory: mapping memory **\n");
	memmap = (uint8_t *)mmap(NULL, getpagesize()*2, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x01C20000);
	if(memmap == NULL) {
        char err[256];
        snprintf(err, sizeof(err), "map_pio_memory: mmap failed (%s)", strerror(errno));
        add_error_msg(err);
        return -1;
	}
	close(fd);

	//Set memmap to point to PIO-registers
	if (DEBUG)
        printf(" ** map_pio_memory: moving to pio registers **\n");
	memmap=memmap+0x800;
	
	return 0;
}

int gpio_get_pud(int port, int pin)
{
	if (DEBUG)
        printf(" ** gpio_get_pud: port %d, pin %d **\n", port, pin);
	volatile uint32_t *pioMem32, *configRegister;
	pioMem32=(uint32_t *)(memmap+port*0x24+0x1c); //0x1c == pull-register
	configRegister=pioMem32+(pin >> 4);
	return *configRegister >> ((pin & 15) * 2) & 3;
}

int gpio_set_pud(int port, int pin, uint8_t value)
{
	if (DEBUG)
        printf(" ** gpio_set_pud: port %d, pin %d, value %d **\n", port, pin, value);
	value &= 3;
	volatile uint32_t *pioMem32, *configRegister;
	uint32_t mask;
	pioMem32=(uint32_t *)(memmap+port*0x24+0x1c); //0x1c == pull-register
	configRegister=pioMem32+(pin >> 4);
	mask = ~(3 << ((pin & 15) * 2));
	*configRegister &= mask;
	*configRegister |= value << ((pin & 15) * 2);
	return 0;
}

int gpio_export(int gpio)
{
    int fd, len, e_no;
    char filename[MAX_FILENAME];
    char str_gpio[80];
    struct gpio_exp *new_gpio, *g;

    if (DEBUG)
        printf(" ** gpio_export **\n");

    snprintf(filename, sizeof(filename), "/sys/class/gpio/export"); BUF2SMALL(filename);

    if ((fd = open(filename, O_WRONLY)) < 0)
    {
        char err[256];
        snprintf(err, sizeof(err), "gpio_export: could not open '%s' (%s)", filename, strerror(errno));
        add_error_msg(err);
        return -1;
    }

    len = snprintf(str_gpio, sizeof(str_gpio), "%d", gpio); BUF2SMALL(str_gpio);
    ssize_t s = write(fd, str_gpio, len);  e_no = errno;
    close(fd);
    if (s != len)
    {
        char err[256];
        snprintf(err, sizeof(err), "gpio_export: could not write '%s' to %s (%s)", str_gpio, filename, strerror(e_no));
        add_error_msg(err);
        return -1;
    }

    // add to list
    if (DEBUG)
        printf(" ** gpio_export: creating data struct **\n");
    new_gpio = malloc(sizeof(struct gpio_exp));  ASSRT(new_gpio != NULL);

    new_gpio->gpio = gpio;
    new_gpio->next = NULL;

    if (exported_gpios == NULL)
    {
        // create new list
        exported_gpios = new_gpio;
    } else {
        // add to end of existing list
        g = exported_gpios;
        while (g->next != NULL)
            g = g->next;
        g->next = new_gpio;
    }

    return 0;
}  /* gpio_export */


void close_value_fd(int gpio)
{
    struct fdx *f = fd_list;
    struct fdx *temp;
    struct fdx *prev = NULL;

    while (f != NULL)
    {
        if (f->gpio == gpio)
        {
            close(f->fd);
            if (prev == NULL)
                fd_list = f->next;
            else
                prev->next = f->next;
            temp = f;
            f = f->next;
            free(temp);
        } else {
            prev = f;
            f = f->next;
        }
    }
}  /* close_value_fd */

int fd_lookup(int gpio)
{
    struct fdx *f = fd_list;
    while (f != NULL)
    {
        if (f->gpio == gpio)
            return f->fd;
        f = f->next;
    }

    return 0;
}

int fde_lookup(int gpio)
{
    struct callback *cb = callbacks;
    while (cb != NULL)
    {
        if (cb->gpio == gpio)
            return cb->fde;
        cb = cb->next;
    }

    return 0;
}

int add_fd_list(int gpio, int fd)
{
    struct fdx *new_fd;

    new_fd = malloc(sizeof(struct fdx));  ASSRT(new_fd != NULL);

    new_fd->fd = fd;
    new_fd->gpio = gpio;
    new_fd->initial = 1;
    new_fd->is_evented = 0;
    if (fd_list == NULL) {
        new_fd->next = NULL;
    } else {
        new_fd->next = fd_list;
    }
    fd_list = new_fd;

    return 0;
}

int open_value_file(int gpio)
{
    int fd;
    char filename[MAX_FILENAME];

    // create file descriptor of value file
    snprintf(filename, sizeof(filename), "/sys/class/gpio/gpio%d/value", gpio); BUF2SMALL(filename);

    // Changed this to open Read/Write to prevent a ton of file open/closes from happening when using
    // the GPIO for SOFTPWM
    if (DEBUG)
        printf(" ** open_value_file **\n");
    if ((fd = open(filename, O_RDWR | O_NONBLOCK)) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "open_value_file: could not open '%s' (%s)", filename, strerror(errno));
        add_error_msg(err);
        return -1;
    }
    add_fd_list(gpio, fd);

    return fd;
}  /* open_value_file */

int open_edge_file(int gpio)
{
    int fd;
    char filename[MAX_FILENAME];

    // create file descriptor of value file
    snprintf(filename, sizeof(filename), "/sys/class/gpio/gpio%d/edge", gpio); BUF2SMALL(filename);

    if (DEBUG)
        printf(" ** open_edge_file **\n");
    if ((fd = open(filename, O_RDONLY | O_NONBLOCK)) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "open_edge_file: could not open '%s' (%s)", filename, strerror(errno));
        add_error_msg(err);
        return -1;
    }

    return fd;
}  /* open_edge_file */

int gpio_unexport(int gpio)
{
    int fd, len, e_no;
    char filename[MAX_FILENAME];
    char str_gpio[16];
    struct gpio_exp *g, *temp, *prev_g = NULL;

    if (DEBUG)
        printf(" ** gpio_unexport **\n");

    close_value_fd(gpio);

    snprintf(filename, sizeof(filename), "/sys/class/gpio/unexport"); BUF2SMALL(filename);

    if ((fd = open(filename, O_WRONLY)) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_unexport: could not open '%s' (%s)", filename, strerror(errno));
        add_error_msg(err);
        return -1;
    }

    len = snprintf(str_gpio, sizeof(str_gpio), "%d", gpio); BUF2SMALL(str_gpio);
    ssize_t s = write(fd, str_gpio, len);  e_no = errno;
    close(fd);
    if (s != len) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_unexport: could not write '%s' (%s)", filename, strerror(e_no));
        add_error_msg(err);
        return -1;
    }

    if (DEBUG)
        printf(" ** gpio_unexport: freeing memory **\n");
    // remove from list
    g = exported_gpios;
    while (g != NULL)
    {
        if (g->gpio == gpio)
        {
            if (prev_g == NULL)
                exported_gpios = g->next;
            else
                prev_g->next = g->next;
            temp = g;
            g = g->next;
            free(temp);
        } else {
            prev_g = g;
            g = g->next;
        }
    }

    return 0;
}

int gpio_set_direction(int gpio, unsigned int in_flag)
{
    int fd, e_no;
    char filename[MAX_FILENAME];  filename[0] = '\0';

    snprintf(filename, sizeof(filename), "/sys/class/gpio/gpio%d/direction", gpio); BUF2SMALL(filename);
    if ((fd = open(filename, O_WRONLY)) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_set_direction: could not open '%s' (%s)", filename, strerror(errno));
        add_error_msg(err);
        return -1;
    }

    char direction[16];
    if (in_flag) {
        strncpy(direction, "out", ARRAY_SIZE(direction) - 1);
    } else {
        strncpy(direction, "in", ARRAY_SIZE(direction) - 1);
    }
    if (DEBUG)
        printf(" ** gpio_set_direction: %s **\n",direction);
    
    ssize_t s = write(fd, direction, strlen(direction));  e_no = errno;
    close(fd);
    if (s != strlen(direction)) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_set_direction: could not write '%s' (%s)", filename, strerror(e_no));
        add_error_msg(err);
        return -1;
    }

    return 0;
}

int gpio_get_direction(int gpio, unsigned int *value)
{
    int fd, e_no;
    char filename[MAX_FILENAME];

    snprintf(filename, sizeof(filename), "/sys/class/gpio/gpio%d/direction", gpio); BUF2SMALL(filename);
    if ((fd = open(filename, O_RDONLY | O_NONBLOCK)) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_get_direction: could not open '%s' (%s)", filename, strerror(errno));
        add_error_msg(err);
        return -1;
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_get_direction: could not seek GPIO %d (%s)", gpio, strerror(errno));
        add_error_msg(err);
        return -1;
    }

    char direction[16] = { 0 };  /* make sure read is null-terminated */
    ssize_t s = read(fd, &direction, sizeof(direction) - 1);  e_no = errno;
    close(fd);
    while (s > 0 && direction[s-1] == '\n') {  /* strip trailing newlines */
        direction[s-1] = '\0';
        s --;
    }
    if (s < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_set_direction: could not read '%s' (%s)", filename, strerror(e_no));
        add_error_msg(err);
        return -1;
    }
    
    if (DEBUG)
        printf(" ** gpio_get_direction: %s **\n",direction);

    if (strcmp(direction, "out") == 0)
        *value = OUTPUT;
    else if (strcmp(direction, "in") == 0)
        *value = INPUT;
    else {
        char err[256];
        snprintf(err, sizeof(err), "gpio_set_direction: unexpected '%s' found in %s", direction, filename);
        add_error_msg(err);
        return -1;
    }

    return 0;
}  /* gpio_set_direction */


int gpio_set_value(int gpio, unsigned int value)
{
    // This now uses the value file descriptor that is set in the other struct
    // in an effort to minimize opening/closing this
    int fd = fd_lookup(gpio);
    int e_no;
    char filename[MAX_FILENAME];
    char vstr[16];

    snprintf(filename, sizeof(filename), "/sys/class/gpio/gpio%d/value", gpio); BUF2SMALL(filename);

    if (!fd)
    {
        if ((fd = open_value_file(gpio)) == -1) {
            char err[256];
            snprintf(err, sizeof(err), "gpio_get_value: could not open GPIO %d value file", gpio);
            add_error_msg(err);
            return -1;
        }
    }

    if (value) {
        strncpy(vstr, "1", ARRAY_SIZE(vstr) - 1);
    } else {
        strncpy(vstr, "0", ARRAY_SIZE(vstr) - 1);
    }

    //if (DEBUG)
    //    printf(" ** gpio_set_value: writing %s **\n", vstr);

    ssize_t s = write(fd, vstr, strlen(vstr));  e_no = errno;

    if (s != strlen(vstr)) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_set_value: could not write '%s' to %s (%s)", vstr, filename, strerror(e_no));
        add_error_msg(err);
        return -2;
    }

    return 0;
}

int gpio_get_value(int gpio, unsigned int *value)
{
    int fd = fd_lookup(gpio);
    char ch;

    if (!fd) {
        if ((fd = open_value_file(gpio)) == -1) {
            char err[256];
            snprintf(err, sizeof(err), "gpio_get_value: could not open GPIO %d value file", gpio);
            add_error_msg(err);
            return -1;
        }
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_get_value: could not seek GPIO %d (%s)", gpio, strerror(errno));
        add_error_msg(err);
        return -1;
    }
    ssize_t s = read(fd, &ch, sizeof(ch));
    if (s < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_get_value: could not read GPIO %d (%s)", gpio, strerror(errno));
        add_error_msg(err);
        return -1;
    }

    if (DEBUG)
        printf(" ** gpio_get_value: %c **\n", ch);

    if (ch == '1') {
        *value = 1;
    } else if (ch == '0') {
        *value = 0;
    } else {
        char err[256];
        snprintf(err, sizeof(err), "gpio_get_value: unrecognized read GPIO %d (%c)", gpio, ch);
        add_error_msg(err);
        return -1;
    }

    return 0;
}

int gpio_get_more(int gpio, int bits, unsigned int *value)
{
    int fd = fd_lookup(gpio);
    char ch;
    
    if (!fd) {
        if ((fd = open_value_file(gpio)) == -1) {
            char err[256];
            snprintf(err, sizeof(err), "gpio_get_more: could not open GPIO %d value file", gpio);
            add_error_msg(err);
            return -1;
        }
    }

    // Loop for our number of bits
    int i;
    for (i = 0; i < bits; i++) {

        if (lseek(fd, 0, SEEK_SET) < 0) {
            char err[256];
            snprintf(err, sizeof(err), "gpio_get_more: could not seek GPIO %d (%s)", gpio, strerror(errno));
            add_error_msg(err);
            return -1;
        }
        ssize_t s = read(fd, &ch, sizeof(ch));
        if (s < 0) {
            char err[256];
            snprintf(err, sizeof(err), "gpio_get_more: could not read GPIO %d (%s)", gpio, strerror(errno));
            add_error_msg(err);
            return -1;
        }
    
        if (ch == '1') {
            *value |= (1 << i);
        } else if (ch == '0') {
            *value |= (0 << i);
        } else {
            char err[256];
            snprintf(err, sizeof(err), "gpio_get_more: unrecognized read GPIO %d (%c)", gpio, ch);
            add_error_msg(err);
            return -1;
        }
        if (DEBUG) {
            printf(" ** gpio_get_more: %c **\n", ch);
            printf(" ** gpio_get_more: current value: %u **\n", *value);
        }
    }

    return 0;
    
}

int gpio_set_edge(int gpio, unsigned int edge)
{
    int fd;
    char filename[MAX_FILENAME];

    snprintf(filename, sizeof(filename), "/sys/class/gpio/gpio%d/edge", gpio); BUF2SMALL(filename);

    if ((fd = open(filename, O_WRONLY)) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_set_edge: could not open '%s' (%s)", filename, strerror(errno));
        add_error_msg(err);
        return -1;
    }

    if (DEBUG)
        printf(" ** gpio_set_edge: %s **\n", stredge[edge]);

    ssize_t s = write(fd, stredge[edge], strlen(stredge[edge]) + 1);
    if (s < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_set_edge: could not write '%s' to %s (%s)", stredge[edge], filename, strerror(errno));
        add_error_msg(err);
        return -1;
    }
    close(fd);

    return 0;
}

int gpio_get_edge(int gpio)
{
    int fd = fde_lookup(gpio);
    int rtnedge = -1;

    if (!fd)
    {
        if ((fd = open_edge_file(gpio)) == -1) {
            char err[256];
            snprintf(err, sizeof(err), "gpio_get_value: could not open GPIO %d edge file", gpio);
            add_error_msg(err);
            return -1;
        }
    }
    
    if (lseek(fd, 0, SEEK_SET) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_get_value: could not seek GPIO %d (%s)", gpio, strerror(errno));
        add_error_msg(err);
        return -1;
    }

    char edge[16] = { 0 };  /* make sure read is null-terminated */
    ssize_t s = read(fd, &edge, sizeof(edge) - 1);
    close(fd);
    while (s > 0 && edge[s-1] == '\n') {  /* strip trailing newlines */
        edge[s-1] = '\0';
        s --;
    }

    if (s < 0) {
        char err[256];
        snprintf(err, sizeof(err), "gpio_get_value: could not read GPIO %d (%s)", gpio, strerror(errno));
        add_error_msg(err);
        return -1;
    }

    if (DEBUG)
        printf(" ** gpio_get_edge: %s **\n", edge);

    if (strcmp(edge, "rising") == 0)
    {
        rtnedge = 1;
    }
    else if (strcmp(edge, "falling") == 0)
    {
        rtnedge = 2;
    }
    else if (strcmp(edge, "both") == 0)
    {
        rtnedge = 3;
    }

    return rtnedge;
}

int gpio_lookup(int fd)
{
    struct fdx *f = fd_list;
    while (f != NULL)
    {
        if (f->fd == fd)
            return f->gpio;
        f = f->next;
    }

    return -1;
}

void exports_cleanup(void)
{
    // unexport everything
    if (DEBUG)
        printf(" ** exports_cleanup **\n");
    while (exported_gpios != NULL)
        gpio_unexport(exported_gpios->gpio);
}

int add_edge_callback(int gpio, int edge, void (*func)(int gpio, void* data), void* data)
{
    struct callback *cb = callbacks;
    struct callback *new_cb;

    if (DEBUG)
        printf(" ** add_edge_callback **\n");
    new_cb = malloc(sizeof(struct callback));  ASSRT(new_cb != NULL);

    new_cb->fde  = open_edge_file(gpio);
    new_cb->gpio = gpio;
    new_cb->edge = edge;
    new_cb->data = data;
    new_cb->func = func;
    new_cb->next = NULL;

    if (callbacks == NULL) {
        // start new list
        callbacks = new_cb;
    } else {
        // add to end of list
        while (cb->next != NULL)
            cb = cb->next;
        cb->next = new_cb;
    }
    return 0;
}

void run_callbacks(int gpio)
{
    struct callback *cb = callbacks;
    while (cb != NULL)
    {
        if (cb->gpio == gpio)
        {
            int canrun = 0;
            unsigned int value = 0;
            gpio_get_value(gpio, &value);
            // Both Edge
            if (cb->edge == 3)
            {
                canrun = 1;
            }
            // Rising Edge
            else if ((cb->edge == 1) && (value == 1))
            {
                canrun = 1;
            }
            // Falling Edge
            else if ((cb->edge == 2) && (value == 0))
            {
                canrun = 1;
            }
            // Only run if we are allowed
            if (canrun)
            {
                if (DEBUG)
                    printf(" ** run_callbacks: gpio triggered: %d **\n", gpio);
                cb->func(cb->gpio, cb->data);
            }

        }
        cb = cb->next;
    }
}

void remove_callbacks(int gpio)
{
    struct callback *cb = callbacks;
    struct callback *temp;
    struct callback *prev = NULL;

    while (cb != NULL)
    {
        if (cb->gpio == gpio)
        {
            if (DEBUG)
                printf(" ** remove_callbacks: gpio: %d **\n", gpio);
            close(cb->fde);
            if (prev == NULL)
                callbacks = cb->next;
            else
                prev->next = cb->next;
            temp = cb;
            cb = cb->next;
            free(temp);
        } else {
            prev = cb;
            cb = cb->next;
        }
    }
}

void set_initial_false(int gpio)
{
    struct fdx *f = fd_list;

    while (f != NULL)
    {
        if (f->gpio == gpio)
            f->initial = 0;
        f = f->next;
    }
}

int gpio_initial(int gpio)
{
    struct fdx *f = fd_list;

    while (f != NULL)
    {
        if ((f->gpio == gpio) && f->initial)
            return 1;
        f = f->next;
    }
    return 0;
}

void *poll_thread(void *threadarg)
{
    struct epoll_event events;
    char buf;
    int gpio;
    int n;

    thread_running = 1;
    while (thread_running)
    {
        if ((n = epoll_wait(epfd, &events, 1, -1)) == -1)
        {
            thread_running = 0;
            pthread_exit(NULL);
        }
        if (n > 0) {
            lseek(events.data.fd, 0, SEEK_SET);
            if (read(events.data.fd, &buf, 1) != 1)
            {
                thread_running = 0;
                pthread_exit(NULL);
            }
            // The return value represents the ending level after the edge.
            gpio = gpio_lookup(events.data.fd);
            if (gpio_initial(gpio)) {     // ignore first epoll trigger
                set_initial_false(gpio);
            } else {
                dyn_int_array_set(&event_occurred, gpio, 1, 0);
                run_callbacks(gpio);
            }
        }
    }
    thread_running = 0;
    pthread_exit(NULL);
}

int gpio_is_evented(int gpio)
{
    struct fdx *f = fd_list;
    while (f != NULL)
    {
        if (f->gpio == gpio)
            return 1;
        f = f->next;
    }
    return 0;
}

int gpio_event_add(int gpio)
{
    struct fdx *f = fd_list;
    while (f != NULL)
    {
        if (f->gpio == gpio)
        {
            if (f->is_evented)
                return 1;

            f->is_evented = 1;
            return 0;
        }
        f = f->next;
    }
    return 0;
}

int gpio_event_remove(int gpio)
{
    struct fdx *f = fd_list;
    while (f != NULL)
    {
        if (f->gpio == gpio)
        {
            f->is_evented = 0;
            return 0;
        }
        f = f->next;
    }
    return 0;
}

// add_edge_detect assumes the caller has ensured the GPIO is already exported.
int add_edge_detect(int gpio, unsigned int edge)
// return values:
// 0 - Success
// 1 - Edge detection already added
// 2 - Other error
{
    int fd = fd_lookup(gpio);
    pthread_t threads;
    struct epoll_event ev;
    long t = 0;

    if (DEBUG)
        printf(" ** add_edge_detect: gpio: %d **\n", gpio);

    // check to see if this gpio has been added already
    if (gpio_event_add(gpio) != 0)
        return 1;

    // export /sys/class/gpio interface
    if (gpio_set_direction(gpio, 0) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "add_edge_detect: could not set direction for GPIO %d", gpio);
        add_error_msg(err);
        return 2;
    }
    if (gpio_set_edge(gpio, edge) < 0) {
        char err[256];
        snprintf(err, sizeof(err), "add_edge_detect: could not set edge for GPIO %d", gpio);
        add_error_msg(err);
        return 2;
    }

    if (!fd)
    {
        if ((fd = open_value_file(gpio)) == -1) {
            char err[256];
            snprintf(err, sizeof(err), "add_edge_detect: could not open GPIO %d value file", gpio);
            add_error_msg(err);
            return 2;
        }
    }

    // create epfd if not already open
    if ((epfd == -1) && ((epfd = epoll_create(1)) == -1)) {
        char err[256];
        snprintf(err, sizeof(err), "add_edge_detect: could not epoll_create GPIO %d (%s)", gpio, strerror(errno));
        add_error_msg(err);
        return 2;
    }

    // add to epoll fd
    ev.events = EPOLLIN | EPOLLET | EPOLLPRI;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        char err[256];
        snprintf(err, sizeof(err), "add_edge_detect: could not epoll_ctl GPIO %d (%s)", gpio, strerror(errno));
        add_error_msg(err);
        return 2;
    }

    // start poll thread if it is not already running
    if (!thread_running)
    {
        if (pthread_create(&threads, NULL, poll_thread, (void *)t) != 0) {
            char err[256];
            snprintf(err, sizeof(err), "add_edge_detect: could not pthread_create GPIO %d (%s)", gpio, strerror(errno));
            add_error_msg(err);
            return 2;
        }
    }

    return 0;
}  /* add_edge_detect */


void remove_edge_detect(int gpio)
{
    struct epoll_event ev;
    int fd = fd_lookup(gpio);

    if (DEBUG)
        printf(" ** remove_edge_detect: gpio : %d **\n", gpio);

    // delete callbacks for gpio
    remove_callbacks(gpio);

    // delete epoll of fd
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);

    // set edge to none
    gpio_set_edge(gpio, NO_EDGE);

    // unexport gpio
    gpio_event_remove(gpio);

    // clear detected flag
    dyn_int_array_set(&event_occurred, gpio, 0, 0);
}


int event_detected(int gpio)
{
    if (dyn_int_array_get(&event_occurred, gpio, 0)) {
        dyn_int_array_set(&event_occurred, gpio, 0, 0);
        return 1;
    } else {
        return 0;
    }
}

void event_cleanup(void)
{
    close(epfd);
    thread_running = 0;
    exports_cleanup();
}

// blocking_wait_for_edge assumes the caller has ensured the GPIO is already exported.
int blocking_wait_for_edge(int gpio, unsigned int edge)
// standalone from all the event functions above
{
    int fd = fd_lookup(gpio);
    int epfd, n, i;
    struct epoll_event events, ev;
    char buf;

    if (DEBUG)
        printf(" ** blocking_wait_for_edge: gpio: %d **\n", gpio);

    if ((epfd = epoll_create(1)) == -1) {
        char err[256];
        snprintf(err, sizeof(err), "blocking_wait_for_edge: could not epoll_create GPIO %d (%s)", gpio, strerror(errno));
        add_error_msg(err);
        return 1;
    }

    // check to see if this gpio has been added already, if not, mark as added
    if (gpio_event_add(gpio) != 0) {
        char err[256];
        snprintf(err, sizeof(err), "blocking_wait_for_edge: could not add event for GPIO %d", gpio);
        add_error_msg(err);
        return 2;
    }

    // export /sys/class/gpio interface
    gpio_set_direction(gpio, 0); // 0=input
    gpio_set_edge(gpio, edge);

    if (!fd)
    {
        if ((fd = open_value_file(gpio)) == -1) {
            char err[256];
            snprintf(err, sizeof(err), "blocking_wait_for_edge: could not open GPIO %d value file", gpio);
            add_error_msg(err);
            return 3;
        }
    }

    // add to epoll fd
    ev.events = EPOLLIN | EPOLLET | EPOLLPRI;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1)
    {
        char err[256];
        snprintf(err, sizeof(err), "blocking_wait_for_edge: could not epoll_ctl GPIO %d (%s)", gpio, strerror(errno));
        add_error_msg(err);
        gpio_event_remove(gpio);
        return 4;
    }

    // epoll for event
    for (i = 0; i<2; i++)  // first time triggers with current state, so ignore
    {
       if ((n = epoll_wait(epfd, &events, 1, -1)) == -1)
       {
           gpio_event_remove(gpio);
           return 5;
       }
    }

    if (n > 0)
    {
        if (lseek(events.data.fd, 0, SEEK_SET) < 0)
        {
            char err[256];
            snprintf(err, sizeof(err), "blocking_wait_for_edge: could not seek GPIO %d (%s)", gpio, strerror(errno));
            add_error_msg(err);
            return 6;
        }
        if (read(events.data.fd, &buf, sizeof(buf)) != 1)
        {
            char err[256];
            snprintf(err, sizeof(err), "blocking_wait_for_edge: could not read GPIO %d (%s)", gpio, strerror(errno));
            add_error_msg(err);
            gpio_event_remove(gpio);
            return 6;
        }
        if (events.data.fd != fd)
        {
            char err[256];
            snprintf(err, sizeof(err), "blocking_wait_for_edge: events.data.fd (%d) not equal to fd (%d) for GPIO %d", events.data.fd, fd, gpio);
            add_error_msg(err);
            gpio_event_remove(gpio);
            return 7;
        }
    }

    if (DEBUG)
        printf(" ** blocking_wait_for_edge: gpio triggered: %d **\n", gpio);

    gpio_event_remove(gpio);
    close(epfd);
    return 0;
}

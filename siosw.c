#include <assert.h>
#include <err.h>
#include <errno.h>
#include <menu.h>
#include <ncurses.h>
#include <poll.h>
#include <sndio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Colour pairs */
#define COLPAIR_MENU_FORE 1
#define COLPAIR_MENU_BACK 2
#define COLPAIR_STATUS 3

#define MENU_WIDTH (COLS - 4)

/* A node in the audio device linked list */
struct sw_dev {
	/* The sndio address of the `device` control */
	unsigned int addr;
	/* The "name" of the device */
	char name[SIOCTL_NAMEMAX];
	/* The display string of the device */
	char *display;
	/* The ncurses menu item for this device */
	ITEM *item;
	/* The next device in the linked list */
	struct sw_dev *next;
};

size_t
num_devs(struct sw_dev *devs)
{
	size_t n = 0;
	while (devs != NULL) {
		n++;
		devs = devs->next;
	}
	return n;
}

/*
 * Frees the constituents of a `struct sw_dev` but not the node itself.
 */
void
free_dev_inner(struct sw_dev *d)
{
	free_item(d->item);
	free(d->display);
}

void
free_devs(struct sw_dev **devs)
{
	struct sw_dev *old;

	while (*devs != NULL) {
		old = *devs;
		free_dev_inner(old);
		*devs = (*devs)->next;
		free(old);
	}
}

void
ondesc_cb(void *arg, struct sioctl_desc *desc, int val)
{
	struct sw_dev **devs = arg, *d;
	(void) val;

	if (desc == NULL)
		return;

	if (!((strcmp(desc->node0.name, "server") == 0) &&
	    (strcmp(desc->func, "device") == 0)))
		return; /* It's not a `server.device` control */

	/* first delete the control */
	for (; *devs != NULL; devs = &d->next) {
		d = *devs;
		if ((*devs)->addr == desc->addr) {
			*devs = d->next;
			free_dev_inner(d);
			free(d);
			break;
		}
	}

	if (desc->type == SIOCTL_NONE)
		return;

	/* otherwise it's a newly appearing device */
	assert(desc->type == SIOCTL_SEL);

	d = malloc(sizeof(struct sw_dev));
	if (d == NULL) {
		endwin();
		err(EXIT_FAILURE, "malloc");
	}

	d->addr = desc->addr;
	strlcpy(d->name, desc->node1.name, SIOCTL_NAMEMAX);
	d->display = malloc(MENU_WIDTH);

	if (d->display == NULL) {
		endwin();
		errx(EXIT_FAILURE, "malloc() failed");
	}
	memset(d->display, ' ', MENU_WIDTH);
	strncpy(d->display, desc->display, strlen(desc->display));
	d->display[MENU_WIDTH] = 0;

	d->item = new_item(d->name, d->display);
	set_item_userptr(d->item, &d->addr);

	d->next = *devs;
	*devs = d;
}

MENU *
create_menu(struct sw_dev *devs)
{
	size_t ndev, i;
	ITEM **items;
	MENU *menu;
	WINDOW *mwin;
	struct sw_dev *d;

	/* Create the menu */
	ndev = num_devs(devs);
	mwin = newwin(
	    ndev,   /* height */
	    MENU_WIDTH, /* width */
	    (LINES - ndev) / 2, /* ypos */
	    (COLS - MENU_WIDTH) / 2 /* xpos */
	);

	items = calloc(ndev + 1, sizeof(ITEM *));
	if (items == NULL) {
		endwin();
		err(EXIT_FAILURE, "malloc");
	}
	for (i = 0, d = devs; d != NULL; d = d->next, i++)
		items[i] = d->item;
	items[ndev] = NULL;

	menu = new_menu(items);
	set_menu_win(menu, mwin);
	set_menu_fore(menu, COLOR_PAIR(COLPAIR_MENU_FORE) | A_REVERSE);
	set_menu_back(menu, COLOR_PAIR(COLPAIR_MENU_BACK) | A_REVERSE);

	return menu;
}

void
sw_free_menu(MENU *menu)
{
	ITEM **items;
	WINDOW *mwin;

	items = menu_items(menu);
	mwin = menu_win(menu);
	free_menu(menu);
	free(items);
	delwin(mwin);
}

void
do_menu(struct sioctl_hdl *hdl)
{
	MENU *menu;
	int exit = 0, poll_rv, nfds;
	struct sw_dev *devs = NULL;
	WINDOW *title_win, *status_win;
	struct pollfd *pfds;

	pfds = malloc(sizeof(struct pollfd) * sioctl_nfds(hdl));
	if (pfds == NULL) {
		endwin();
		err(EXIT_FAILURE, "malloc");
	}

	if (sioctl_ondesc(hdl, ondesc_cb, &devs) == 0) {
		endwin();
		errx(EXIT_FAILURE, "sioctl_desc() failed");
	}

	/* Create the bar at the top */
	title_win = newwin(1, COLS, 0, 0);
	wbkgd(title_win, COLOR_PAIR(COLPAIR_STATUS));
	mvwprintw(title_win, 0, 0, "Select default sndio device");

	menu = create_menu(devs);
	post_menu(menu);

	/* Create the bar at the bottom */
	status_win = newwin(1, COLS, LINES - 1, 0);
	wbkgd(status_win, COLOR_PAIR(COLPAIR_STATUS));
	mvwprintw(status_win, 0, 0,
	    "[Return]: change audio device, [q]: exit");

	refresh();
	wrefresh(title_win);
	wrefresh(menu_win(menu));
	wrefresh(status_win);

	while(!exit) {
		/* Check for sndio device changes */
		nfds = sioctl_pollfd(hdl, pfds, POLLIN);
		while ((poll_rv = poll(pfds, nfds, 100)) < 0) {
			if (errno != EINTR) {
				endwin();
				err(EXIT_FAILURE, "poll");
			}
		}
		if (poll_rv > 0) {
			/* Something changed. Repopulate the menu */
			unpost_menu(menu);
			wrefresh(menu_win(menu));
			sw_free_menu(menu);

			 /* Update the device list (calls `ondesc_cb`) */
			sioctl_revents(hdl, pfds);

			menu = create_menu(devs);
			post_menu(menu);
			wrefresh(menu_win(menu));
			refresh();
		}

		switch(getch()) {
			case ERR:
				/* no keypresses available */
				continue;
			case KEY_DOWN:
				menu_driver(menu, REQ_DOWN_ITEM);
				break;
			case KEY_UP:
				menu_driver(menu, REQ_UP_ITEM);
				break;
			case '\n':
				if (sioctl_setval(hdl, *((unsigned int *)
				    item_userptr(current_item(menu))), 1) == 0)
				{
					endwin();
					errx(EXIT_FAILURE,
					    "sioctl_setval() failed");
				}
				exit = 1;
				break;
			case 'q':
				mvprintw(LINES - 2, 0, "q");
				exit = 1;
				break;
		}
		wrefresh(menu_win(menu));
	}

	unpost_menu(menu);
	sw_free_menu(menu);
	delwin(status_win);
	delwin(title_win);
	free_devs(&devs);
}

int
main(int argc, char **argv)
{
	struct sioctl_hdl *hdl = NULL;
	(void) argc;
	(void) argv;

	hdl = sioctl_open(SIO_DEVANY, SIOCTL_WRITE, 0);
	if (hdl == NULL)
		errx(EXIT_FAILURE, "sioctl_open() failed");

	initscr();
	cbreak();
	noecho();
	curs_set(0);
	keypad(stdscr, TRUE);
	timeout(0); /* non-blocking getch() */

	start_color();
	init_pair(COLPAIR_MENU_FORE, COLOR_YELLOW, COLOR_BLACK);
	init_pair(COLPAIR_MENU_BACK, COLOR_WHITE, COLOR_BLACK);
	init_pair(COLPAIR_STATUS, COLOR_BLACK, COLOR_BLUE);

	do_menu(hdl);

	endwin();
	sioctl_close(hdl);

	return EXIT_SUCCESS;
}

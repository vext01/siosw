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

/* The `state` of the menu, passed to (and mutated by) sioctl callbacks */
struct sw_state {
	/* Linked list of devices */
	struct sw_dev *devs;
	/* Pointer to the status window at the bottom of the screen */
	WINDOW *status_win;
	/* The currently active default device */
	struct sw_dev *cur;
};

size_t
sw_num_devs(struct sw_dev *devs)
{
	size_t n = 0;
	while (devs != NULL) {
		n++;
		devs = devs->next;
	}
	return n;
}

void
sw_update_status(WINDOW *status_win, char *dev)
{
	wclear(status_win);
	mvwprintw(status_win, 0, 0, "Currently selected device: %s", dev);
	wrefresh(status_win);
}

void
sw_free_dev(struct sw_dev *d)
{
	free_item(d->item);
	free(d->display);
	free(d);
}

void
sw_free_devs(struct sw_dev *devs)
{
	struct sw_dev *next;

	while (devs != NULL) {
		next = devs->next;
		sw_free_dev(devs);
		devs = next;
	}
}

struct sw_dev *
sw_new_dev(struct sioctl_desc *desc)
{
	struct sw_dev *d;

	d = malloc(sizeof(struct sw_dev));
	if (d == NULL) {
		endwin();
		err(EXIT_FAILURE, "malloc() failed");
	}

	d->addr = desc->addr;
	strlcpy(d->name, desc->node1.name, SIOCTL_NAMEMAX);

	d->display = malloc(MENU_WIDTH);
	if (d->display == NULL) {
		endwin();
		err(EXIT_FAILURE, "malloc() failed");
	}
	memset(d->display, ' ', MENU_WIDTH);
	strncpy(d->display, desc->display, strlen(desc->display));
	d->display[MENU_WIDTH - 1] = 0;

	d->item = new_item(d->name, d->display);
	set_item_userptr(d->item, &d->addr);

	return d;
}

void
sw_ondesc_cb(void *arg, struct sioctl_desc *desc, int val)
{
	struct sw_state *state = arg;
	struct sw_dev **devs = &state->devs, *d;
	(void) val;

	if (desc == NULL)
		return;

	if (!((strcmp(desc->node0.name, "server") == 0) &&
	    (strcmp(desc->func, "device") == 0)))
		return; /* It's not a `server.device` control */

	/* first delete the control if it already exists */
	for (; *devs != NULL; devs = &d->next) {
		d = *devs;
		if ((*devs)->addr == desc->addr) {
			*devs = d->next;
			sw_free_dev(d);
			break;
		}
	}

	if (desc->type == SIOCTL_NONE)
		return; /* device is being deleted, so don't recreate */

	/* create or recreate the control */
	assert(desc->type == SIOCTL_SEL);
	d = sw_new_dev(desc);
	d->next = *devs;
	*devs = d;

	/* If it's the active device, then update the status window */
	if (val) {
		sw_update_status(state->status_win, d->display);
		state->cur = d;
	}
}

void
sw_onval_cb(void *arg, unsigned addr, unsigned val)
{
	struct sw_state *state = arg;
	struct sw_dev *d;

	/* See if the update is a change of default audio device */
	for (d = state->devs; d != NULL; d = d->next) {
		if ((d->addr == addr) && (val)) {
			sw_update_status(state->status_win, d->display);
			state->cur = d;
			break;
		}
	}
	/* If we get here, it was an upate for a control we don't care about */
}

MENU *
sw_create_menu(struct sw_state *state)
{
	size_t ndev, i;
	ITEM **items;
	MENU *menu;
	WINDOW *mwin;
	struct sw_dev *d;

	/* Create the menu */
	ndev = sw_num_devs(state->devs);
	mwin = newwin(
	    ndev,   /* height */
	    MENU_WIDTH, /* width */
	    (LINES - ndev) / 2, /* ypos */
	    (COLS - MENU_WIDTH) / 2 /* xpos */
	);

	items = calloc(ndev + 1, sizeof(ITEM *));
	if (items == NULL) {
		endwin();
		err(EXIT_FAILURE, "calloc() failed");
	}
	for (i = 0, d = state->devs; d != NULL; d = d->next, i++)
		items[i] = d->item;
	items[ndev] = NULL;

	menu = new_menu(items);
	set_menu_win(menu, mwin);
	set_menu_sub(menu, mwin);
	set_menu_fore(menu, COLOR_PAIR(COLPAIR_MENU_FORE) | A_REVERSE);
	set_menu_back(menu, COLOR_PAIR(COLPAIR_MENU_BACK) | A_REVERSE);
	set_current_item(menu, state->cur->item);

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
sw_do_menu(struct sioctl_hdl *hdl)
{
	MENU *menu;
	int exit = 0, poll_rv, sio_nfds, nfds, i, again;
	WINDOW *title_win, *status_win;
	struct pollfd *pfds, *sio_pfds;
	struct sw_state state;

	refresh();

	/* Create the bar at the top */
	title_win = newwin(1, COLS, 0, 0);
	wbkgd(title_win, COLOR_PAIR(COLPAIR_STATUS));
	mvwprintw(title_win, 0, 0, "Select default sndio device");
	wrefresh(title_win);

	/* Create the bar at the bottom */
	status_win = newwin(1, COLS, LINES - 1, 0);
	wbkgd(status_win, COLOR_PAIR(COLPAIR_STATUS));

	state.devs = NULL; /* empty linked list */
	state.status_win = status_win;

	if (sioctl_ondesc(hdl, sw_ondesc_cb, &state) == 0) {
		endwin();
		errx(EXIT_FAILURE, "sioctl_ondesc() failed");
	}

	if (sioctl_onval(hdl, sw_onval_cb, &state) == 0) {
		endwin();
		errx(EXIT_FAILURE, "sioctl_onval() failed");
	}

	/* Draw the first incarnation of the menu */
	menu = sw_create_menu(&state);
	post_menu(menu);
	wrefresh(menu_win(menu));

	sio_nfds = sioctl_nfds(hdl);
	nfds = sio_nfds + 1; /* +1 for stdin at index 0 */
	pfds = malloc(sizeof(struct pollfd) * nfds);
	if (pfds == NULL) {
		endwin();
		err(EXIT_FAILURE, "malloc() failed");
	}
	sio_pfds = pfds + 1;

	pfds[0].fd = STDIN_FILENO;
	pfds[0].events = POLLIN;
	pfds[0].revents = 0;

	while(!exit) {
		/* Check for key events or sndio device changes */
		sioctl_pollfd(hdl, sio_pfds, POLLIN);
		while ((poll_rv = poll(pfds, nfds, INFTIM)) < 0) {
			if (errno != EINTR) {
				endwin();
				err(EXIT_FAILURE, "poll() failed");
			}
		}

		again = 0;

		/*
		 * `poll()` woke up, if it was because of any sio
		 * device change, then we have to update the menu.
		 */
		for (i = 0; i < sio_nfds; i++) {
			if (sio_pfds[i].revents & POLLHUP) {
				endwin();
				errx(EXIT_FAILURE, "lost connection to sndiod");
			} else if (sio_pfds[i].revents & POLLIN) {
				/* Device changed. Repopulate menu */
				unpost_menu(menu);
				wrefresh(menu_win(menu));
				sw_free_menu(menu);

				/*
				 * This could be optimised by only updating the
				 * menu once we see an invocation of the
				 * callback with a NULL arg (thus marking the
				 * end of a batch of updates.
				 *
				 * It hardly seeems worth it for this program.
				 */
				sioctl_revents(hdl, sio_pfds);

				menu = sw_create_menu(&state);
				post_menu(menu);
				wrefresh(menu_win(menu));
				refresh();

				again = 1;
				break;
			}
		}

		/*
		 * If the devices changed, re-run the menu to avoid the race
		 * where the user hits enter on a device which subsequently
		 * changed.
		 */
		if (again)
			continue;

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
				break;
			case 'q':
			case 27: /* escape */
				exit = 1;
				break;
		}
		wrefresh(menu_win(menu));
	}

	unpost_menu(menu);
	sw_free_menu(menu);
	delwin(status_win);
	delwin(title_win);
	sw_free_devs(state.devs);
	free(pfds);
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
	ESCDELAY = 0;

	start_color();
	init_pair(COLPAIR_MENU_FORE, COLOR_YELLOW, COLOR_BLACK);
	init_pair(COLPAIR_MENU_BACK, COLOR_WHITE, COLOR_BLACK);
	init_pair(COLPAIR_STATUS, COLOR_BLACK, COLOR_BLUE);

	sw_do_menu(hdl);

	endwin();
	sioctl_close(hdl);

	return EXIT_SUCCESS;
}

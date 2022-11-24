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

/* The maximum number of devices that can appear in the menu */
#define SW_MAXDEV 16

/* Colour pairs */
#define COLPAIR_MENU_FORE 1
#define COLPAIR_MENU_BACK 2
#define COLPAIR_STATUS 3

#define MENU_WIDTH (COLS - 4)

/* Table of audio devices */
struct sw_devs {
	/* Number of entries */
	unsigned int num;
	/* sndio control addresses */
	unsigned int addrs[SW_MAXDEV];
	/* sndio device names */
	char names[SW_MAXDEV][SIOCTL_NAMEMAX];
	/* device display strings */
	char *displays[SW_MAXDEV];
	/* ncurses menu items */
	ITEM *items[SW_MAXDEV];
};

void
ondesc_cb(void *arg, struct sioctl_desc *desc, int val)
{
	struct sw_devs *devs = arg;
	(void) val;

	/* XXX this should add/delete controls like `sndioctl -m` does */

	/* Note that we need space for a NULL sentinel in `devs->items` */
	if ((desc == NULL) || (devs->num == SW_MAXDEV - 1))
		return;

	if ((desc->type == SIOCTL_SEL) &&
	    (strcmp(desc->node0.name, "server") == 0) &&
	    (strcmp(desc->func, "device") == 0))
	{
		devs->addrs[devs->num] = desc->addr;
		strlcpy(devs->names[devs->num], desc->node1.name,
		    SIOCTL_NAMEMAX);

		devs->displays[devs->num] = malloc(MENU_WIDTH);
		if (devs->displays[devs->num] == NULL) {
			endwin();
			errx(EXIT_FAILURE, "malloc() failed");
		}
		memset(devs->displays[devs->num], ' ', MENU_WIDTH);
		strncpy(devs->displays[devs->num],desc->display,
		    strlen(desc->display));
		devs->displays[devs->num][MENU_WIDTH] = 0;

		devs->items[devs->num] = new_item(devs->names[devs->num],
		    devs->displays[devs->num]);
		set_item_userptr(devs->items[devs->num],
		    &devs->addrs[devs->num]);

		devs->num++;
	}
}

void
clear_devs(struct sw_devs *devs) {
	unsigned int i;

	for (i = 0; i < devs->num; i++) {
		free_item(devs->items[i]);
		free(devs->displays[i]);
	}
	memset(devs, 0, sizeof(struct sw_devs));
}

void
do_menu(struct sioctl_hdl *hdl, char *argv0)
{
	MENU *menu;
	int exit = 0, nfds, poll_rv;
	struct sw_devs devs;
	WINDOW *title_win, *menu_win, *status_win;
	struct pollfd *pfds;

	pfds = malloc(sizeof(struct pollfd) * sioctl_nfds(hdl));
	if (pfds == NULL) {
		endwin();
		err(EXIT_FAILURE, "malloc");
	}

	memset(&devs, 0, sizeof(struct sw_devs));
	if (sioctl_ondesc(hdl, ondesc_cb, &devs) == 0) {
		endwin();
		errx(EXIT_FAILURE, "sioctl_desc() failed");
	}

	if (devs.num == 0) {
		endwin();
		errx(EXIT_FAILURE, "no viable audio devices");
	}

	/* Create the bar at the top */
	title_win = newwin(1, COLS, 0, 0);
	wbkgd(title_win, COLOR_PAIR(COLPAIR_STATUS));
	mvwprintw(title_win, 0, 0, "Select default sndio device");

	/* Create the menu */
	menu_win = newwin(
	    devs.num,   /* height */
	    MENU_WIDTH, /* width */
	    (LINES - devs.num) / 2, /* ypos */
	    (COLS - MENU_WIDTH) / 2 /* xpos */
	);
	menu = new_menu(devs.items);
	set_menu_win(menu, menu_win);
	set_menu_fore(menu, COLOR_PAIR(COLPAIR_MENU_FORE) | A_REVERSE);
	set_menu_back(menu, COLOR_PAIR(COLPAIR_MENU_BACK) | A_REVERSE);
	post_menu(menu);

	/* Create the bar at the bottom */
	status_win = newwin(1, COLS, LINES - 1, 0);
	wbkgd(status_win, COLOR_PAIR(COLPAIR_STATUS));
	mvwprintw(status_win, 0, 0,
	    "[Return]: change audio device, [r] refresh list, [q]: exit");

	refresh();
	wrefresh(title_win);
	wrefresh(menu_win);
	wrefresh(status_win);

	while(!exit) {
		/* Check for sndio control changes */
		system("notify-send loop");
		nfds = sioctl_pollfd(hdl, pfds, POLLIN);
		while ((poll_rv = poll(pfds, nfds, 100)) < 0) {
			system("notify-send minus one");
			if (errno != EINTR) {
				endwin();
				err(EXIT_FAILURE, "poll");
			}
		}
		if (poll_rv > 0) {
			/* Something changed. Repopulate the menu */
			system("notify-send new");
			unpost_menu(menu);
			free_menu(menu);
			clear_devs(&devs); /* XXX wrong */
			sioctl_revents(hdl, pfds);
			if (devs.num == 0) {
				endwin();
				errx(EXIT_FAILURE, "no devices"); /* XXX don't do this */
			}

			/* XXX duplication */
			menu = new_menu(devs.items);
			set_menu_win(menu, menu_win);
			set_menu_fore(menu, COLOR_PAIR(COLPAIR_MENU_FORE) | A_REVERSE);
			set_menu_back(menu, COLOR_PAIR(COLPAIR_MENU_BACK) | A_REVERSE);
			post_menu(menu);
			wrefresh(menu_win);
		}

		// Drive the menu.
		switch(getch()) {
			case KEY_DOWN:
				menu_driver(menu, REQ_DOWN_ITEM);
				break;
			case KEY_UP:
				menu_driver(menu, REQ_UP_ITEM);
				break;
			case 'r':
				execl(argv0, argv0, NULL);
				endwin();
				err(EXIT_FAILURE, "execl() failed");
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
		wrefresh(menu_win);
	}

	clear_devs(&devs);
	unpost_menu(menu);
	free_menu(menu);
}

int
main(int argc, char **argv)
{
	struct sioctl_hdl *hdl = NULL;
	(void) argc;

	hdl = sioctl_open(SIO_DEVANY, SIOCTL_WRITE, 0);
	if (hdl == NULL)
		errx(EXIT_FAILURE, "sioctl_open() failed");

	initscr();
	cbreak();
	noecho();
	curs_set(0);
	keypad(stdscr, TRUE);

	start_color();
	init_pair(COLPAIR_MENU_FORE, COLOR_YELLOW, COLOR_BLACK);
	init_pair(COLPAIR_MENU_BACK, COLOR_WHITE, COLOR_BLACK);
	init_pair(COLPAIR_STATUS, COLOR_BLACK, COLOR_BLUE);

	do_menu(hdl, argv[0]);

	endwin();
	sioctl_close(hdl);

	return EXIT_SUCCESS;
}

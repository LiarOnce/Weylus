#include "xwindows.h"
#include "error.h"
#include <X11/X.h>
#include <stdio.h>
#include <stdlib.h>

int locale_to_utf8(char* src, char* dest, size_t size)
{
	iconv_t icd = iconv_open("UTF-8//IGNORE", "");
	size_t src_size = strlen(src);
	size_t inbytes = src_size;
	size_t outbytes_left = MAX_PROPERTY_VALUE_LEN - 1;
	int ret = iconv(icd, &src, &src_size, &dest, &outbytes_left);
	iconv_close(icd);
	if (ret < 0)
	{
		return -1;
	}
	dest[src_size - 1 - outbytes_left] = '\0';
	return 0;
}

char* get_property(
	Display* disp, Window win, Atom xa_prop_type, char* prop_name, unsigned long* size, Error* err)
{
	Atom xa_prop_name;
	Atom xa_ret_type;
	int ret_format;
	unsigned long ret_nitems;
	unsigned long ret_bytes_after;
	unsigned long tmp_size;
	unsigned char* ret_prop;
	char* ret;

	xa_prop_name = XInternAtom(disp, prop_name, False);

	/* MAX_PROPERTY_VALUE_LEN / 4 explanation (XGetWindowProperty manpage):
	 *
	 * long_length = Specifies the length in 32-bit multiples of the
	 *               data to be retrieved.
	 */
	if (XGetWindowProperty(
			disp,
			win,
			xa_prop_name,
			0,
			MAX_PROPERTY_VALUE_LEN / 4,
			False,
			xa_prop_type,
			&xa_ret_type,
			&ret_format,
			&ret_nitems,
			&ret_bytes_after,
			&ret_prop) != Success)
	{
		fill_error(err, 1, "Cannot get %s property.\n", prop_name);
		return NULL;
	}

	if (xa_ret_type != xa_prop_type)
	{
		fill_error(err, 1, "Invalid type of %s property.\n", prop_name);
		XFree(ret_prop);
		return NULL;
	}

	/* null terminate the result to make string handling easier */
	tmp_size = (ret_format / 8) * ret_nitems;
	/* Correct 64 Architecture implementation of 32 bit data */
	if (ret_format == 32)
		tmp_size *= sizeof(long) / 4;
	ret = malloc(tmp_size + 1);
	memcpy(ret, ret_prop, tmp_size);
	ret[tmp_size] = '\0';

	if (size)
	{
		*size = tmp_size;
	}

	XFree(ret_prop);
	return ret;
}

char* get_window_title(Display* disp, Window win, Error* err)
{
	char* title_utf8;
	char* wm_name;
	char* net_wm_name;
	Error err_wm;
	Error err_net_wm;

	wm_name = get_property(disp, win, XA_STRING, "WM_NAME", NULL, &err_wm);
	net_wm_name = get_property(
		disp, win, XInternAtom(disp, "UTF8_STRING", False), "_NET_WM_NAME", NULL, &err_net_wm);

	if (net_wm_name)
	{
		title_utf8 = strdup(net_wm_name);
	}
	else
	{
		if (wm_name)
		{
			title_utf8 = malloc(MAX_PROPERTY_VALUE_LEN);
			if (locale_to_utf8(wm_name, title_utf8, MAX_PROPERTY_VALUE_LEN) != 0)
			{
				fill_error(err, 1, "Failed to convert windowname to UTF-8!");
				free(title_utf8);
				title_utf8 = NULL;
			}
		}
		else
		{
			fill_error(
				err,
				1,
				"Could not get window name: (%s) (%s)",
				err_net_wm.error_str,
				err_wm.error_str);
			title_utf8 = NULL;
		}
	}

	free(wm_name);
	free(net_wm_name);

	return title_utf8;
}

Window* get_client_list(Display* disp, unsigned long* size, Error* err)
{
	Window* client_list;
	Error err_net;
	Error err_win;
	if ((client_list = (Window*)get_property(
			 disp, DefaultRootWindow(disp), XA_WINDOW, "_NET_CLIENT_LIST", size, &err_net)) == NULL)
	{
		if ((client_list = (Window*)get_property(
				 disp, DefaultRootWindow(disp), XA_CARDINAL, "_WIN_CLIENT_LIST", size, &err_win)) ==
			NULL)
		{
			fill_error(
				err,
				1,
				"Cannot get client list properties. "
				"_NET_CLIENT_LIST: %s or _WIN_CLIENT_LIST: %s",
				err_net.error_str,
				err_win.error_str);
			return NULL;
		}
	}

	return client_list;
}

void free_window_info(WindowInfo* windows, size_t size)
{
	for (int i = 0; i < size; ++i)
	{
		free(windows[i].title);
	}
	free(windows->win);
}

size_t create_window_info(Display* disp, WindowInfo** window_info, Error* err)
{
	Window* client_list;
	unsigned long client_list_size;
	int max_client_machine_len = 0;

	if ((client_list = get_client_list(disp, &client_list_size, err)) == NULL)
	{
		return 0;
	}

	size_t num_windows = client_list_size / sizeof(Window);
	WindowInfo* windows = malloc(num_windows * sizeof(WindowInfo));

	/* print the list */
	for (int i = 0; i < num_windows; i++)
	{
		char* title_utf8 = get_window_title(disp, client_list[i], NULL);
		if (title_utf8 == NULL)
		{
			title_utf8 = malloc(16);
			snprintf(title_utf8, 16, "UNKNOWN %d", i);
		}
		unsigned long* desktop;

		/* desktop ID */
		if ((desktop = (unsigned long*)get_property(
				 disp, client_list[i], XA_CARDINAL, "_NET_WM_DESKTOP", NULL, NULL)) == NULL)
		{
			desktop = (unsigned long*)get_property(
				disp, client_list[i], XA_CARDINAL, "_WIN_WORKSPACE", NULL, NULL);
		}

		/* special desktop ID -1 means "all desktops", so we
		   have to convert the desktop value to signed long */
		printf(
			"0x%.8lx %2ld %s\n", client_list[i], desktop ? (signed long)*desktop : 0, title_utf8);

		windows[i].disp = disp;

		// remeber to free those:
		windows[i].win = &client_list[i];
		// use -2 to indicate that we have no clue
		windows[i].desktop_id = desktop ? *desktop : -2;
		windows[i].title = title_utf8;

		*window_info = windows;

		free(desktop);
	}

	return num_windows;
}

void get_window_geometry(
	WindowInfo* winfo, int* x, int* y, unsigned int* width, unsigned int* height, Error* err)
{
	Window junkroot;
	int junkx, junky;
	unsigned int bw, depth;
	if (!XGetGeometry(
			winfo->disp, *winfo->win, &junkroot, &junkx, &junky, width, height, &bw, &depth))
	{
		ERROR(err, 1, "Failed to get window geometry!");
	}
	XTranslateCoordinates(winfo->disp, *winfo->win, junkroot, junkx, junky, x, y, &junkroot);
}
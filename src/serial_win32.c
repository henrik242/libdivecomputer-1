/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdlib.h>

#define NOGDI
#include <windows.h>

#include "serial.h"

#include "common-private.h"
#include "context-private.h"
#include "iostream-private.h"

static dc_status_t dc_serial_set_timeout (dc_iostream_t *iostream, int timeout);
static dc_status_t dc_serial_set_latency (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_set_halfduplex (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_set_break (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_set_dtr (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_set_rts (dc_iostream_t *iostream, unsigned int value);
static dc_status_t dc_serial_get_lines (dc_iostream_t *iostream, unsigned int *value);
static dc_status_t dc_serial_get_available (dc_iostream_t *iostream, size_t *value);
static dc_status_t dc_serial_configure (dc_iostream_t *iostream, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol);
static dc_status_t dc_serial_read (dc_iostream_t *iostream, void *data, size_t size, size_t *actual);
static dc_status_t dc_serial_write (dc_iostream_t *iostream, const void *data, size_t size, size_t *actual);
static dc_status_t dc_serial_flush (dc_iostream_t *iostream);
static dc_status_t dc_serial_purge (dc_iostream_t *iostream, dc_direction_t direction);
static dc_status_t dc_serial_sleep (dc_iostream_t *iostream, unsigned int milliseconds);
static dc_status_t dc_serial_close (dc_iostream_t *iostream);

typedef struct dc_serial_t {
	dc_iostream_t base;
	/*
	 * The file descriptor corresponding to the serial port.
	 */
	HANDLE hFile;
	/*
	 * Serial port settings are saved into this variables immediately
	 * after the port is opened. These settings are restored when the
	 * serial port is closed.
	 */
	DCB dcb;
	COMMTIMEOUTS timeouts;
	/* Half-duplex settings */
	int halfduplex;
	unsigned int baudrate;
	unsigned int nbits;
} dc_serial_t;

static const dc_iostream_vtable_t dc_serial_vtable = {
	sizeof(dc_serial_t),
	dc_serial_set_timeout, /* set_timeout */
	dc_serial_set_latency, /* set_latency */
	dc_serial_set_halfduplex, /* set_halfduplex */
	dc_serial_set_break, /* set_break */
	dc_serial_set_dtr, /* set_dtr */
	dc_serial_set_rts, /* set_rts */
	dc_serial_get_lines, /* get_lines */
	dc_serial_get_available, /* get_received */
	dc_serial_configure, /* configure */
	dc_serial_read, /* read */
	dc_serial_write, /* write */
	dc_serial_flush, /* flush */
	dc_serial_purge, /* purge */
	dc_serial_sleep, /* sleep */
	dc_serial_close, /* close */
};

static dc_status_t
syserror(DWORD errcode)
{
	switch (errcode) {
	case ERROR_INVALID_PARAMETER:
		return DC_STATUS_INVALIDARGS;
	case ERROR_OUTOFMEMORY:
		return DC_STATUS_NOMEMORY;
	case ERROR_FILE_NOT_FOUND:
		return DC_STATUS_NODEVICE;
	case ERROR_ACCESS_DENIED:
		return DC_STATUS_NOACCESS;
	default:
		return DC_STATUS_IO;
	}
}

dc_status_t
dc_serial_enumerate (dc_serial_callback_t callback, void *userdata)
{
	// Open the registry key.
	HKEY hKey;
	LONG rc = RegOpenKeyExA (HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE, &hKey);
	if (rc != ERROR_SUCCESS) {
		if (rc == ERROR_FILE_NOT_FOUND)
			return DC_STATUS_SUCCESS;
		else
			return DC_STATUS_IO;
	}

	// Get the number of values.
	DWORD count = 0;
	rc = RegQueryInfoKey (hKey, NULL, NULL, NULL, NULL, NULL, NULL, &count, NULL, NULL, NULL, NULL);
	if (rc != ERROR_SUCCESS) {
		RegCloseKey(hKey);
		return DC_STATUS_IO;
	}

	for (DWORD i = 0; i < count; ++i) {
		// Get the value name, data and type.
		char name[512], data[512];
		DWORD name_len = sizeof (name);
		DWORD data_len = sizeof (data);
		DWORD type = 0;
		rc = RegEnumValueA (hKey, i, name, &name_len, NULL, &type, (LPBYTE) data, &data_len);
		if (rc != ERROR_SUCCESS) {
			RegCloseKey(hKey);
			return DC_STATUS_IO;
		}

		// Ignore non-string values.
		if (type != REG_SZ)
			continue;

		// Prevent a possible buffer overflow.
		if (data_len >= sizeof (data)) {
			RegCloseKey(hKey);
			return DC_STATUS_NOMEMORY;
		}

		// Null terminate the string.
		data[data_len] = 0;

		callback (data, userdata);
	}

	RegCloseKey(hKey);

	return DC_STATUS_SUCCESS;
}

dc_status_t
dc_serial_open (dc_iostream_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = NULL;

	if (out == NULL || name == NULL)
		return DC_STATUS_INVALIDARGS;

	INFO (context, "Open: name=%s", name);

	// Build the device name.
	const char *devname = NULL;
	char buffer[MAX_PATH] = "\\\\.\\";
	if (strncmp (name, buffer, 4) != 0) {
		size_t length = strlen (name) + 1;
		if (length + 4 > sizeof (buffer))
			return DC_STATUS_NOMEMORY;
		memcpy (buffer + 4, name, length);
		devname = buffer;
	} else {
		devname = name;
	}

	// Allocate memory.
	device = (dc_serial_t *) dc_iostream_allocate (context, &dc_serial_vtable);
	if (device == NULL) {
		SYSERROR (context, ERROR_OUTOFMEMORY);
		return DC_STATUS_NOMEMORY;
	}

	// Default to full-duplex.
	device->halfduplex = 0;
	device->baudrate = 0;
	device->nbits = 0;

	// Open the device.
	device->hFile = CreateFileA (devname,
			GENERIC_READ | GENERIC_WRITE, 0,
			NULL, // No security attributes.
			OPEN_EXISTING,
			0, // Non-overlapped I/O.
			NULL);
	if (device->hFile == INVALID_HANDLE_VALUE) {
		DWORD errcode = GetLastError ();
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_free;
	}

	// Retrieve the current communication settings and timeouts,
	// to be able to restore them when closing the device.
	// It is also used to check if the obtained handle
	// represents a serial device.
	if (!GetCommState (device->hFile, &device->dcb) ||
		!GetCommTimeouts (device->hFile, &device->timeouts)) {
		DWORD errcode = GetLastError ();
		SYSERROR (context, errcode);
		status = syserror (errcode);
		goto error_close;
	}

	*out = (dc_iostream_t *) device;

	return DC_STATUS_SUCCESS;

error_close:
	CloseHandle (device->hFile);
error_free:
	dc_iostream_deallocate ((dc_iostream_t *) device);
	return status;
}

static dc_status_t
dc_serial_close (dc_iostream_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = (dc_serial_t *) abstract;

	// Restore the initial communication settings and timeouts.
	if (!SetCommState (device->hFile, &device->dcb) ||
		!SetCommTimeouts (device->hFile, &device->timeouts)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		dc_status_set_error(&status, syserror (errcode));
	}

	// Close the device.
	if (!CloseHandle (device->hFile)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		dc_status_set_error(&status, syserror (errcode));
	}

	return status;
}

static dc_status_t
dc_serial_configure (dc_iostream_t *abstract, unsigned int baudrate, unsigned int databits, dc_parity_t parity, dc_stopbits_t stopbits, dc_flowcontrol_t flowcontrol)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	// Retrieve the current settings.
	DCB dcb;
	if (!GetCommState (device->hFile, &dcb)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	dcb.fBinary = TRUE; // Enable Binary Transmission
	dcb.fAbortOnError = FALSE;

	// Baudrate.
	dcb.BaudRate = baudrate;

	// Character size.
	if (databits >= 5 && databits <= 8)
		dcb.ByteSize = databits;
	else
		return DC_STATUS_INVALIDARGS;

	// Parity checking.
	switch (parity) {
	case DC_PARITY_NONE:
		dcb.Parity = NOPARITY;
		dcb.fParity = FALSE;
		break;
	case DC_PARITY_EVEN:
		dcb.Parity = EVENPARITY;
		dcb.fParity = TRUE;
		break;
	case DC_PARITY_ODD:
		dcb.Parity = ODDPARITY;
		dcb.fParity = TRUE;
		break;
	case DC_PARITY_MARK:
		dcb.Parity = MARKPARITY;
		dcb.fParity = TRUE;
		break;
	case DC_PARITY_SPACE:
		dcb.Parity = SPACEPARITY;
		dcb.fParity = TRUE;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Stopbits.
	switch (stopbits) {
	case DC_STOPBITS_ONE:
		dcb.StopBits = ONESTOPBIT;
		break;
	case DC_STOPBITS_ONEPOINTFIVE:
		dcb.StopBits = ONE5STOPBITS;
		break;
	case DC_STOPBITS_TWO:
		dcb.StopBits = TWOSTOPBITS;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Flow control.
	switch (flowcontrol) {
	case DC_FLOWCONTROL_NONE:
		dcb.fInX = FALSE;
		dcb.fOutX = FALSE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDtrControl = DTR_CONTROL_ENABLE;
		dcb.fRtsControl = RTS_CONTROL_ENABLE;
		break;
	case DC_FLOWCONTROL_HARDWARE:
		dcb.fInX = FALSE;
		dcb.fOutX = FALSE;
		dcb.fOutxCtsFlow = TRUE;
		dcb.fOutxDsrFlow = TRUE;
		dcb.fDtrControl = DTR_CONTROL_HANDSHAKE;
		dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
		break;
	case DC_FLOWCONTROL_SOFTWARE:
		dcb.fInX = TRUE;
		dcb.fOutX = TRUE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDtrControl = DTR_CONTROL_ENABLE;
		dcb.fRtsControl = RTS_CONTROL_ENABLE;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	// Apply the new settings.
	if (!SetCommState (device->hFile, &dcb)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	device->baudrate = baudrate;
	device->nbits = 1 + databits + stopbits + (parity ? 1 : 0);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_timeout (dc_iostream_t *abstract, int timeout)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	// Retrieve the current timeouts.
	COMMTIMEOUTS timeouts;
	if (!GetCommTimeouts (device->hFile, &timeouts)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	// Update the settings.
	if (timeout < 0) {
		// Blocking mode.
		timeouts.ReadIntervalTimeout = 0;
		timeouts.ReadTotalTimeoutMultiplier = 0;
		timeouts.ReadTotalTimeoutConstant = 0;
		timeouts.WriteTotalTimeoutMultiplier = 0;
		timeouts.WriteTotalTimeoutConstant = 0;
	} else if (timeout == 0) {
		// Non-blocking mode.
		timeouts.ReadIntervalTimeout = MAXDWORD;
		timeouts.ReadTotalTimeoutMultiplier = 0;
		timeouts.ReadTotalTimeoutConstant = 0;
		timeouts.WriteTotalTimeoutMultiplier = 0;
		timeouts.WriteTotalTimeoutConstant = 0;
	} else {
		// Standard timeout mode.
		timeouts.ReadIntervalTimeout = 0;
		timeouts.ReadTotalTimeoutMultiplier = 0;
		timeouts.ReadTotalTimeoutConstant = timeout;
		timeouts.WriteTotalTimeoutMultiplier = 0;
		timeouts.WriteTotalTimeoutConstant = 0;
	}

	// Activate the new timeouts.
	if (!SetCommTimeouts (device->hFile, &timeouts)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_halfduplex (dc_iostream_t *abstract, unsigned int value)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	device->halfduplex = value;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_latency (dc_iostream_t *abstract, unsigned int value)
{
	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_read (dc_iostream_t *abstract, void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = (dc_serial_t *) abstract;
	DWORD dwRead = 0;

	if (!ReadFile (device->hFile, data, size, &dwRead, NULL)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		status = syserror (errcode);
		goto out;
	}

	if (dwRead != size) {
		status = DC_STATUS_TIMEOUT;
	}

out:
	if (actual)
		*actual = dwRead;

	return status;
}

static dc_status_t
dc_serial_write (dc_iostream_t *abstract, const void *data, size_t size, size_t *actual)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_serial_t *device = (dc_serial_t *) abstract;
	DWORD dwWritten = 0;

	LARGE_INTEGER begin, end, freq;
	if (device->halfduplex) {
		// Get the current time.
		if (!QueryPerformanceFrequency(&freq) ||
			!QueryPerformanceCounter(&begin)) {
			DWORD errcode = GetLastError ();
			SYSERROR (abstract->context, errcode);
			status = syserror (errcode);
			goto out;
		}
	}

	if (!WriteFile (device->hFile, data, size, &dwWritten, NULL)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		status = syserror (errcode);
		goto out;
	}

	if (device->halfduplex) {
		// Get the current time.
		if (!QueryPerformanceCounter(&end))  {
			DWORD errcode = GetLastError ();
			SYSERROR (abstract->context, errcode);
			status = syserror (errcode);
			goto out;
		}

		// Calculate the elapsed time (microseconds).
		unsigned long elapsed = 1000000.0 * (end.QuadPart - begin.QuadPart) / freq.QuadPart + 0.5;

		// Calculate the expected duration (microseconds). A 2 millisecond fudge
		// factor is added because it improves the success rate significantly.
		unsigned long expected = 1000000.0 * device->nbits / device->baudrate * size + 0.5 + 2000;

		// Wait for the remaining time.
		if (elapsed < expected) {
			unsigned long remaining = expected - elapsed;

			// The remaining time is rounded up to the nearest millisecond
			// because the Windows Sleep() function doesn't have a higher
			// resolution.
			dc_serial_sleep (abstract, (remaining + 999) / 1000);
		}
	}

	if (dwWritten != size) {
		status = DC_STATUS_TIMEOUT;
	}

out:
	if (actual)
		*actual = dwWritten;

	return status;
}

static dc_status_t
dc_serial_purge (dc_iostream_t *abstract, dc_direction_t direction)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	DWORD flags = 0;

	switch (direction) {
	case DC_DIRECTION_INPUT:
		flags = PURGE_RXABORT | PURGE_RXCLEAR;
		break;
	case DC_DIRECTION_OUTPUT:
		flags = PURGE_TXABORT | PURGE_TXCLEAR;
		break;
	case DC_DIRECTION_ALL:
		flags = PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_TXCLEAR;
		break;
	default:
		return DC_STATUS_INVALIDARGS;
	}

	if (!PurgeComm (device->hFile, flags)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_flush (dc_iostream_t *abstract)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	if (!FlushFileBuffers (device->hFile)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_break (dc_iostream_t *abstract, unsigned int level)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	if (level) {
		if (!SetCommBreak (device->hFile)) {
			DWORD errcode = GetLastError ();
			SYSERROR (abstract->context, errcode);
			return syserror (errcode);
		}
	} else {
		if (!ClearCommBreak (device->hFile)) {
			DWORD errcode = GetLastError ();
			SYSERROR (abstract->context, errcode);
			return syserror (errcode);
		}
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_dtr (dc_iostream_t *abstract, unsigned int level)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	int status = (level ? SETDTR : CLRDTR);

	if (!EscapeCommFunction (device->hFile, status)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_set_rts (dc_iostream_t *abstract, unsigned int level)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	int status = (level ? SETRTS : CLRRTS);

	if (!EscapeCommFunction (device->hFile, status)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_get_available (dc_iostream_t *abstract, size_t *value)
{
	dc_serial_t *device = (dc_serial_t *) abstract;

	COMSTAT stats;

	if (!ClearCommError (device->hFile, NULL, &stats)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	if (value)
		*value = stats.cbInQue;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_get_lines (dc_iostream_t *abstract, unsigned int *value)
{
	dc_serial_t *device = (dc_serial_t *) abstract;
	unsigned int lines = 0;

	DWORD stats = 0;
	if (!GetCommModemStatus (device->hFile, &stats)) {
		DWORD errcode = GetLastError ();
		SYSERROR (abstract->context, errcode);
		return syserror (errcode);
	}

	if (stats & MS_RLSD_ON)
		lines |= DC_LINE_DCD;
	if (stats & MS_CTS_ON)
		lines |= DC_LINE_CTS;
	if (stats & MS_DSR_ON)
		lines |= DC_LINE_DSR;
	if (stats & MS_RING_ON)
		lines |= DC_LINE_RNG;

	if (value)
		*value = lines;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
dc_serial_sleep (dc_iostream_t *abstract, unsigned int timeout)
{
	Sleep (timeout);

	return DC_STATUS_SUCCESS;
}

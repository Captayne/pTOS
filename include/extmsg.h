/*
 * extmsg.h - how a device gets a message to a GEM application
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This software is licenced under the GNU Public License.
 * Please see LICENSE.TXT for further information.
 *
 * Keyboards and mice reach the AES through the standard input path.  A
 * device with something else to say -- a real-time task reporting a
 * result, say -- has no such path, and reaching into the AES from the
 * outside is not one either: its message pipes and event blocks are
 * touched from the scheduler's own context, with no caller to block and
 * no lock to take.
 *
 * So the AES asks instead.  A device leaves a hook here; the AES calls
 * it from its dispatcher, between processes, where posting a message is
 * safe and nothing waits.  A device that has nothing pending says so and
 * costs a comparison.
 *
 * The hook returns the process id to deliver to (as appl_init() gave it
 * to the application) and fills a standard 16-byte, eight-word AES
 * message.  It returns -1 when there is nothing.  It is called
 * repeatedly until it does.
 *
 * It runs in the AES dispatcher, so it must not block, must not call
 * GEMDOS, and must be quick: whatever it takes comes off every process
 * on the system core.
 */

#ifndef EXTMSG_H
#define EXTMSG_H

extern WORD (*aes_extmsg)(WORD *msg);

#endif /* EXTMSG_H */

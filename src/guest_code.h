#ifndef GUEST_CODE_H
#define GUEST_CODE_H

static const unsigned char guest_code[] = {
    0xba,
    0xf8,
    0x03, /* mov $0x3f8, %dx    -- dx = COM1 I/O port */
    0x00,
    0xd8, /* add %bl, %al       -- al += bl */
    0x04,
    '0',  /* add $'0' (0x30), %al  -- convert to ASCII digit */
    0xee, /* out %al, (%dx)     -- write digit to serial */
    0xb0,
    '\n', /* mov $'\n', %al */
    0xee, /* out %al, (%dx)     -- write newline */
    0xf4, /* hlt */
};

#endif // GUEST_CODE_H
#ifndef RNET_OPEN_URL_H
#define RNET_OPEN_URL_H

/* Launch an HTTP(S) URL in the host browser. Returns 0 if no opener could be
 * started. This does not wait for a browser session to finish. */
int rnet_open_url(const char *url);

#endif

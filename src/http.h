#ifndef TRAYCOIN_HTTP_H
#define TRAYCOIN_HTTP_H

#include "types.h"
#include <winhttp.h>

typedef struct {
    DWORD  statusCode;   /* HTTP status code (200, 429, 404...), or 0 on transport error */
    char  *body;         /* Heap-allocated null-terminated UTF-8 response body */
    DWORD  bodyLen;
    DWORD  winError;     /* GetLastError() if transport failed */
} HttpResponse;

int       http_init(void);
void      http_shutdown(void);
HINTERNET http_get_session(void);

/* Perform HTTPS GET request. extraHeaders may be NULL (e.g. L"X-CMC_PRO_API_KEY: abc\r\n"). */
int  http_get(const wchar_t *host, const wchar_t *path, const wchar_t *extraHeaders, HttpResponse *out);
void http_free(HttpResponse *res);

/* Percent-encode ASCII/UTF-8 string for URL query parameters. */
void http_url_encode_w(const wchar_t *in, wchar_t *out, int outCap);

#endif /* TRAYCOIN_HTTP_H */

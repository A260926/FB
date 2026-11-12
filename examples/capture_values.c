//
//  capture_values.c
//  Katana
//
//  Offline resource-capture example. Given a declaration list already
//  matched against one element (ordered low to high cascade priority),
//  resolve the custom properties and list the decoded resource URLs of a
//  normal property. No network access happens here.
//
//  Usage:
//    ./capture_values "<declaration list>" <property>
//
//  Example:
//    ./capture_values \
//      "--hero: url(a.png); background-image: var(--hero, url(b.png));" \
//      background-image
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture_values.h"

int main(int argc, const char* argv[]) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: %s \"<declaration list>\" <property>\n", argv[0]);
        return 2;
    }

    const char* css = argv[1];
    const char* property = argv[2];

    KatanaResolvedStyle* style =
        katana_resolve_style(css, strlen(css), NULL);

    const char* value = katana_resolved_value(style, property);
    if (value == NULL) {
        printf("%s: <invalid at computed-value time or absent>\n",
               property);
    } else {
        printf("%s: %s\n", property, value);
    }

    const KatanaArray* urls = katana_resolved_urls(style, property);
    if (urls == NULL) {
        printf("urls: <unavailable: property absent or invalid>\n");
    } else if (urls->length == 0) {
        printf("urls: <none>\n");
    } else {
        for (unsigned i = 0; i < urls->length; i++)
            printf("url[%u]: %s\n", i, (const char*)urls->data[i]);
    }

    // The value strings and the URL array are all freed here; nothing may
    // be used after this call.
    katana_resolved_style_destroy(style);
    return 0;
}

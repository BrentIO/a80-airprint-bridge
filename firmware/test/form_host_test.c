// Host test for formparse.h: gcc -Wall -Werror -o /tmp/form_test form_host_test.c && /tmp/form_test
#include <assert.h>
#include <stdio.h>
#include "../main/formparse.h"

int main(void)
{
    char o[16];
    assert(form_get("a=1&b=2", "a", o, sizeof o) == 1 && !strcmp(o, "1"));
    assert(form_get("a=1&b=2", "b", o, sizeof o) == 1 && !strcmp(o, "2"));
    assert(form_get("a=1&b=2", "c", o, sizeof o) == -1 && !o[0]);
    assert(form_get("a=1&b=2", "", o, sizeof o) == -1);
    // plus and percent decoding, upper and lower case hex
    assert(form_get("ssid=My+Home%20Wi-Fi", "ssid", o, sizeof o) == 13 && !strcmp(o, "My Home Wi-Fi"));
    assert(form_get("p=a%26b%3Dc%2b%25", "p", o, sizeof o) == 7 && !strcmp(o, "a&b=c+%"));
    // a key that is a prefix of another key must not match
    assert(form_get("ssid2=x&ssid=y", "ssid", o, sizeof o) == 1 && !strcmp(o, "y"));
    assert(form_get("xssid=x", "ssid", o, sizeof o) == -1);
    // empty value
    assert(form_get("pass=&pick=A", "pass", o, sizeof o) == 0 && !o[0]);
    assert(form_get("pass=&pick=A", "pick", o, sizeof o) == 1 && !strcmp(o, "A"));
    // bad or cut-off percent escapes are kept as typed
    assert(form_get("a=100%", "a", o, sizeof o) == 4 && !strcmp(o, "100%"));
    assert(form_get("a=%zz", "a", o, sizeof o) == 3 && !strcmp(o, "%zz"));
    assert(form_get("a=%4", "a", o, sizeof o) == 2 && !strcmp(o, "%4"));
    assert(form_get("a=%41&b=%42", "a", o, sizeof o) == 1 && !strcmp(o, "A"));
    // too long: reports the real length, output is cut and terminated
    char s[5];
    assert(form_get("k=abcdefgh", "k", s, sizeof s) == 8 && !strcmp(s, "abcd"));
    // UTF-8 SSID bytes
    assert(form_get("s=caf%C3%A9", "s", o, sizeof o) == 5 && !strcmp(o, "caf\xC3\xA9"));
    puts("form parsing OK");
    return 0;
}

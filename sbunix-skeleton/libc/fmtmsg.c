#include <fmtmsg.h>

#include <stdio.h>

int fmtmsg(long classification, const char *label, int severity,
           const char *text, const char *action, const char *tag) {
  (void)classification;
  (void)severity;

  if (label)
    fprintf(stderr, "%s: ", label);
  if (text)
    fprintf(stderr, "%s", text);
  if (action)
    fprintf(stderr, " TO FIX: %s", action);
  if (tag)
    fprintf(stderr, " (%s)", tag);
  fprintf(stderr, "\n");
  return MM_OK;
}

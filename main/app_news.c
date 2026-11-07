// News: BBC World headlines, through the RSS reader in app_hackaday.c.
#include "badge.h"

void rss_reader(const char *url, const char *name); // app_hackaday.c

void app_news(void)
{
    rss_reader("https://feeds.bbci.co.uk/news/world/rss.xml", "BBC World");
}

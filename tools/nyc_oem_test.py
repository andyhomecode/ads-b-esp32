#!/usr/bin/env python3
"""Proof-of-concept for pulling NYC OEM (Notify NYC) emergency alerts.

Not part of the firmware build - a standalone exploration script for whether/how
to add a "current NYC emergency" feed to the device, alongside the existing NWS
alert feed (see src/main.cpp).

Two independent sources, evaluated here:

1. SOCRATA HISTORICAL DATASET - reference only, not for live alerting.
   Dataset page: https://data.cityofnewyork.us/Public-Safety/NYCEM-Emergency-Notifications/8vv7-7wx3
   API endpoint: https://data.cityofnewyork.us/resource/8vv7-7wx3.json
   Socrata SoQL query language ($where, $order, $limit, $select):
     https://dev.socrata.com/docs/queries/

   - Schema confirmed via the dataset's own published data dictionary
     (attachment "OEM_Emergency_Notifications_Data_Dictionary.xlsx", downloaded
     from https://data.cityofnewyork.us/api/assets/<blobId>?download=true - the
     blobId comes from the dataset's metadata.attachments, fetched via the
     Socrata "views" metadata API: https://data.cityofnewyork.us/api/views/8vv7-7wx3.json).
     It documents exactly 5 columns: record_id, date_and_time, notificationtype,
     notification_title, email_body. There is NO severity/urgency/priority
     column - "Domain Values"/"Expected Values" for notificationtype are blank
     in the dictionary, i.e. NYC never published an enum for it either.
   - Notify NYC's own FAQ (https://a858-nycnotify.nyc.gov/Home/FAQ) confirms why:
     "Determination of incident severity is at the discretion of the Public
     Warning Specialist and is based upon the projected impacts of an incident
     due to location, size, time of day, and number of people affected." I.e.
     severity is a human judgment call at send-time, never captured as data -
     it only shows up (if at all) as free text in the title/body.
   - The real notificationtype values (checked via
     $select=distinct notificationtype) do NOT include "Emergency Alert" or
     "Public Health Notification" - those don't exist in this dataset. Real
     values: Emergency Activity, Public Health, Weather, Local Mass Transit,
     Regional Mass Transit, Mass Transit Restoration, Transportation, Utility,
     Planned Events, School Notification, Missing Person, Nixle,
     zINAC * Drills / Exercises, zINAC * Fire, zINAC * Environmental,
     zINAC * Aerial (Fly-Over), zINAC * Parking.
   - The dataset is stale: as of 2026-09-12, max(date_and_time) across all
     ~31k rows is 2025-09-15, despite the dataset metadata claiming
     "Update Frequency: Daily". NYC Open Data community reports (found via web
     search) describe the automated feed behind this dataset breaking and not
     yet being restored - so treat this source as historical only.

2. LIVE CAP (Common Alerting Protocol) RSS FEED - the actual source for
   real-time, severity-tagged alerts.
   Feed URL: https://feeds.everbridge.net/feeds/453003085617722/rss/rss.xml
   Discovered via the Notify NYC homepage footer
   (https://a858-nycnotify.nyc.gov/), which links it as "RSS". Confirmed live
   (not archived) because its <lastBuildDate> tracks the real clock, unlike the
   Socrata dataset above.

   - This is served by Everbridge (NYC's alerting vendor); see their docs on the
     CAP RSS publishing feature:
       https://supportcenter.everbridge.com/hc/en-us/articles/19141926845467-EBS-CAP-RSS-Feed-Publishing-Option-in-Everbridge-Suite
       https://supportcenter.everbridge.com/hc/en-us/articles/29360868045851-EBS-Publishing-an-Everbridge-Notification-to-a-CAP-RSS-Feed
     (both are Zendesk-gated behind a login when fetched directly - summarized
     via search results rather than read in full.)
   - The general (non-Everbridge-specific) OASIS "CAP-feeds" v1.0 spec explains
     the wrapping convention used here:
       https://docs.oasis-open.org/emergency-adopt/cap-feeds/v1.0/cap-feeds-v1.0.html
     Per that spec: item/title, item/pubDate, item/category, item/description
     map to cap:alert:info:headline/sent/category/description directly - but
     severity, urgency, certainty, and event are NOT inline in the RSS <item>.
     Each item instead links to the full CAP XML document (recommended via
     <enclosure type="application/cap+xml">, though this feed may instead put
     the CAP doc URL straight in <link> - both are handled below), which must
     be fetched separately to get those fields.
   - The CAP schema itself (element names, and the enumerated values for
     severity/urgency/certainty) is the OASIS CAP v1.2 standard:
       https://docs.oasis-open.org/emergency/cap/v1.2/CAP-v1.2-os.html
     Same standard NWS alerts use (api.weather.gov wraps CAP in GeoJSON; this
     feed gives raw CAP XML instead) - see [[device-external-apis]] memory /
     the NWS alert code in src/main.cpp for the existing device-side CAP
     consumer. Valid <severity> values: Extreme, Severe, Moderate, Minor,
     Unknown. Valid <urgency> values: Immediate, Expected, Future, Past,
     Unknown. Valid <certainty> values: Observed, Likely, Possible, Unlikely,
     Unknown. "Truly high urgency" for a future device feature would mean
     filtering on urgency in (Immediate, Expected) and severity in
     (Extreme, Severe).
   - CAVEAT: at test time (2026-09-12) the feed had zero items (no active NYC
     alert), so the CAP-document parsing path (fetch_cap_alerts /
     _cap_doc_url) was verified with a hand-built synthetic RSS item + CAP XML
     doc, not a real one end-to-end. Re-run `--cap` next time NYC has an
     active alert to confirm the enclosure/link detection picks the right URL
     in practice.
"""
import argparse
import datetime
import sys
import xml.etree.ElementTree as ET

import requests

BASE_URL = "https://data.cityofnewyork.us/resource/8vv7-7wx3.json"
CAP_RSS_URL = "https://feeds.everbridge.net/feeds/453003085617722/rss/rss.xml"
CAP_NS = "urn:oasis:names:tc:emergency:cap:1.2"  # OASIS CAP v1.2, see module docstring

# Types that plausibly matter for an "active emergency" display.
# "zINAC *" prefixed types are internal/drill notifications - excluded.
EMERGENCY_TYPES = [
    "Emergency Activity",
    "Public Health",
    "Weather",
]


def fetch_alerts(since: datetime.datetime, types=EMERGENCY_TYPES, limit=20):
    """Query the (stale, reference-only) Socrata dataset. SoQL syntax:
    https://dev.socrata.com/docs/queries/"""
    type_list = ",".join(f"'{t}'" for t in types)
    where = (
        f"notificationtype in({type_list}) "
        f"AND date_and_time >= '{since.strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3]}'"
    )
    params = {
        "$where": where,
        "$order": "date_and_time DESC",
        "$limit": str(limit),
    }
    resp = requests.get(BASE_URL, params=params, timeout=10)
    resp.raise_for_status()
    return resp.json()


def _cap_doc_url(item: ET.Element):
    """Per the OASIS CAP-feeds v1.0 spec, an RSS item wrapping a CAP alert
    points at the full CAP XML via <enclosure type="application/cap+xml"
    url="..."/>; some CAP-feed implementations instead put the CAP doc URL
    directly in <link>. Try enclosure first, then link.
    Spec: https://docs.oasis-open.org/emergency-adopt/cap-feeds/v1.0/cap-feeds-v1.0.html
    """
    enclosure = item.find("enclosure")
    if enclosure is not None and "cap" in (enclosure.get("type") or ""):
        return enclosure.get("url")
    link = item.find("link")
    if link is not None and link.text:
        return link.text.strip()
    return None


def fetch_cap_alerts(limit=20):
    """Query the live Everbridge CAP RSS feed, following each item out to its
    full CAP XML document for severity/urgency/certainty (not present inline -
    see module docstring and OASIS CAP v1.2:
    https://docs.oasis-open.org/emergency/cap/v1.2/CAP-v1.2-os.html)."""
    resp = requests.get(CAP_RSS_URL, timeout=10)
    resp.raise_for_status()
    root = ET.fromstring(resp.content)
    items = root.findall("./channel/item")[:limit]

    alerts = []
    for item in items:
        title = (item.findtext("title") or "").strip()
        pub_date = (item.findtext("pubDate") or "").strip()
        cap_url = _cap_doc_url(item)

        info = {"title": title, "pubDate": pub_date, "cap_url": cap_url}

        if cap_url:
            cap_resp = requests.get(cap_url, timeout=10)
            cap_resp.raise_for_status()
            cap_root = ET.fromstring(cap_resp.content)

            def cap_find(path):
                # Every path segment needs the CAP namespace prefix, not just
                # the first one, or ElementTree silently finds nothing.
                qualified = "/".join(f"{{{CAP_NS}}}{part}" for part in path.split("/"))
                return cap_root.findtext(f".//{qualified}")

            info["event"] = cap_find("info/event")
            info["severity"] = cap_find("info/severity")
            info["urgency"] = cap_find("info/urgency")
            info["certainty"] = cap_find("info/certainty")
            info["headline"] = cap_find("info/headline")
            info["effective"] = cap_find("info/effective")
            info["expires"] = cap_find("info/expires")

        alerts.append(info)
    return alerts


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--cap", action="store_true",
        help="query the live CAP RSS feed instead of the stale Socrata dataset",
    )
    ap.add_argument(
        "--days", type=float, default=400,
        help="(Socrata mode) lookback window in days (default 400, since the feed is stale)",
    )
    ap.add_argument(
        "--types", nargs="*", default=EMERGENCY_TYPES,
        help="(Socrata mode) notificationtype values to include",
    )
    ap.add_argument("--limit", type=int, default=20)
    args = ap.parse_args()

    if args.cap:
        print(f"Querying {CAP_RSS_URL}", file=sys.stderr)
        alerts = fetch_cap_alerts(args.limit)
        if not alerts:
            print("No items in CAP feed right now (this is expected when NYC has no active alert).")
            return
        for a in alerts:
            print(f"[{a.get('pubDate')}] {a.get('title')}")
            if "severity" in a:
                print(f"    event={a.get('event')} severity={a.get('severity')} "
                      f"urgency={a.get('urgency')} certainty={a.get('certainty')}")
                print(f"    effective={a.get('effective')} expires={a.get('expires')}")
            print()
        return

    since = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=args.days)
    print(f"Querying since {since.isoformat()}Z, types={args.types}", file=sys.stderr)

    alerts = fetch_alerts(since, args.types, args.limit)

    if not alerts:
        print("No alerts found in window.")
        return

    for a in alerts:
        print(f"[{a.get('date_and_time')}] ({a.get('notificationtype')}) "
              f"{a.get('notification_title')}")
        body = a.get("email_body", "")
        if body:
            print(f"    {body[:200]}{'...' if len(body) > 200 else ''}")
        print()


if __name__ == "__main__":
    main()

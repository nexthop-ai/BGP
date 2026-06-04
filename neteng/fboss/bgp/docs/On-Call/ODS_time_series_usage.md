# Oncall Guide: ODS time series usage

`bgp++` is the major user of the
[FBOSS ODS category 284](https://www.internalfb.com/intern/ods/category?cat_id=284).
The `Routing Protocol` oncall is expected to analyze and reach out to other
oncall if issue arises.

# 1. ODS time series usage analysis

> Please visit
> [FBOSS ODS category 284](https://www.internalfb.com/intern/ods/category?cat_id=284)
> for simple management and summary usage

The ODS time series usage of this
[FBOSS ODS category 284](https://www.internalfb.com/intern/ods/category?cat_id=284)
is available on [ods_gorilla_unique](https://fburl.com/data/w3ufgeki) hive
table. We can use [Daiquery](https://www.internalfb.com/intern/wiki/Daiquery/)
to analyze it. It usually takes 3 days for it to be available for querying
there.

## 1.1 time series count by `key_pattern`

Here's the
[Daiquery example](https://www.internalfb.com/intern/daiquery/?queryid=494821296742358)
result. You can change the `query_date` on that sample.

> raw P1546072946

| key_pattern    | num_entities |   num_keys | num_timeseries |
| :------------- | -----------: | ---------: | -------------: |
| openr.%        |      309,907 |  1,614,119 |    156,328,029 |
| qsfp_service.% |      353,978 |     37,880 |    948,256,987 |
| fboss.bgp.%    |      349,039 |  7,859,186 |  1,074,340,438 |
| <any>          |      389,670 | 11,555,186 |  4,400,617,683 |

We can calculate

1. `free` slice `=`
   [time series count limit](https://www.internalfb.com/code/configerator/[b1c63c4bf698][history]/source/ods/categories/fboss.ods_category.cconf?lines=29)` - <any>`
2. `others` slice = `<any> - (<fboss.bgp.%> + <qsfp_service.%> + <openr.%>)`

### Below is the pie chart from the query and calculation

![Limit 6,930,000 time series](https://www.internalfb.com/intern/wiki/_download/?title=8a82dffc-170e-4377-898a-29b2c7822ed1%23%20time%20series%2C%20Aug%2020th%2C%202024.png)

## 1.2 time series count grouped by certain `key_pattern`

Here's the
[Daiquery example](https://www.internalfb.com/intern/daiquery/?queryid=521263540352638)
of `fboss.bgp.peer_%.postfilterAdvtPfxLen` key pattern on Aug 20th, 2024 result.
You can change the `query_date` and `key_pattern` on the example.

> raw P1546074706

| key                                                       | num_entities | total_timeseries |
| :-------------------------------------------------------- | -----------: | ---------------: |
| fboss.bgp.peer_bgp_monitor:2.postfilterAdvtPfxLen         |      305,014 |          318,991 |
| fboss.bgp.peer_bgp_monitor:1.postfilterAdvtPfxLen         |        4,144 |            6,016 |
| fboss.bgp.peer_vipinjector:v6:5.postfilterAdvtPfxLen      |          507 |            1,245 |
| fboss.bgp.peer_vipinjector:v6:11.postfilterAdvtPfxLen     |          489 |            1,191 |
| fboss.bgp.peer_vipinjector:v6:4.postfilterAdvtPfxLen      |          423 |            1,161 |
| fboss.bgp.peer_vipinjector:v6:12.postfilterAdvtPfxLen     |          403 |            1,033 |
| fboss.bgp.peer_vipinjector:v6:10.postfilterAdvtPfxLen     |          230 |              644 |
| fboss.bgp.peer_vipinjector:v6:2.postfilterAdvtPfxLen      |          229 |              643 |
| fboss.bgp.peer_jsw004.m004.zgd1:v4:1.postfilterAdvtPfxLen |          363 |              552 |
| fboss.bgp.peer_jsw006.m004.zgd1:v6:1.postfilterAdvtPfxLen |          363 |              552 |
| fboss.bgp.peer_jsw005.m004.zgd1:v6:1.postfilterAdvtPfxLen |          363 |              552 |

# 2. FBOSS time series is missing

> We have hit it in [S353172](https://www.internalfb.com/sevmanager/view/353172)
> and [S429396](https://www.internalfb.com/sevmanager/view/429396)

If **ANY** time series is missing, not just `fboss.bgp.*` or `'openr.*` keys,
verify the time series count whether it goes beyond the FBOSS limit or not

1. Check the time series count usage on [scuba](https://fburl.com/ods/6rjrz6nf)
   for the FBOSS ODS category 284. In this
   [ODS category 284 management web page](https://www.internalfb.com/intern/ods/category?cat_id=284),
   there is a column showing `Time Series Count` within last 26 hours and the
   `Current Limit`. The `Current Limit` is in this
   [configerator](https://www.internalfb.com/code/configerator/[b1c63c4bf698][history]/source/ods/categories/fboss.ods_category.cconf?lines=29).
   Do not forget the Configerator CodeHub to the latest revision.
   - If the current usage is below 90% of the `Current Limit`, then we can rule
     this out

2. If the current usage is >= 90% of the `Current Limit`, reach out to
   [fboss_agent oncall](https://www.internalfb.com/omh/view/fboss_agent/oncall_profile)
   immediately and ask if they have noticed this or not. If not, ask the
   [fboss_agent oncall](https://www.internalfb.com/omh/view/fboss_agent/oncall_profile)
   to open a SEV and own it. They will reach out to
   [ODS_storage oncall](https://www.internalfb.com/omh/view/ods_storage/oncall_profile)
   to mitigate the SEV.
   - It is likely that the
     [fboss_agent oncall](https://www.internalfb.com/omh/view/fboss_agent/oncall_profile)
     would get an ODS alert and fired a SEV prior to Routing oncall find this
     manually.

3. The
   [ODS oncall](https://www.internalfb.com/omh/view/ods_storage/oncall_profile)
   knows exactly what needs to be done. Here's the brief summary of what will
   happen
   - (Ask)
     [ODS oncall](https://www.internalfb.com/omh/view/ods_storage/oncall_profile)
     to provide emergency quota grants.
     - example [D59088168](https://www.internalfb.com/diff/D59088168)

   - Apply blocklist any **unwanted** new counters or timeseries being emitted
     on the ODS configerator's side, by using the blocklisting tools on the
     category page
     - example [D59502832](https://www.internalfb.com/diff/D59502832)
     - Track the ODS spam blocklist on
       [scuba](https://fburl.com/scuba/ods_blocklist_usage/vttsbg3k)

   - On the FBOSS monitoring side, 1. remove the configuration `regex` that pick
     those **unwanted** `fb303` counters, or, 2. move those counters into the
     `blacklist`
     - example [D59526358](https://www.internalfb.com/diff/D59526358)

# 3. `ods eki` CLI

There are some `ODS` commands those are useful to check the production FBOSS
device's ODS keys and time series.

> Nevertheless, the result is NOT as accurate as `Daiquery`

## examples

3.1 show how many different keys (with `fboss.bgp` pattern) are collected from
`fa002-du031.cco2`.

```sh
ods eki --entity fa002-du031.cco2 --key_partial "fboss.bgp" --show_cardinality
# 1403 total matches.
```

> Note: 1403 keys do not mean 1403 time series

3.2 show how many time series in `category-id` `284` are generated by each key.

> Each key is _exact_ match, not `regex` key pattern

```sh
ods eki --category-id 284 --key_regex_search "fboss\.bgp." --sort_by_time_series_counts --limit 10
```

```sh
# result
fboss.bgp.bgpd.config.peers.ucmp_advertise.disable (358674)
fboss.bgp.bgpd.config.peers.ucmp_advertise.best_path (358667)
fboss.bgp.bgpd.config.peers.ucmp_receive.accept (358657)
fboss.bgp.bgpd.eorTimerExpired (358652)
fboss.bgp.bgpd.configuredPeers (358647)
fboss.bgp.bgpd.config.peers.ucmp_receive.disable (358634)
fboss.bgp.bgpd.config.peers.ucmp_advertise.set_link_bps (358621)
fboss.bgp.bgpd.config.peers.ucmp_advertise.aggregate_local (358616)
fboss.bgp.bgpd.config.peers.ucmp_advertise.rib_policy_lbw (358604)
fboss.bgp.bgpd.config.peers.ucmp_advertise.aggregate_received (358603)
fboss.bgp.bgpd.config.ucmp_enabled (213297)
```

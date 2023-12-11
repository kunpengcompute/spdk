# LAVA_RPC

## Overview

SPDK implements a [JSON-RPC 2.0](http:/www.jsonrpc.org/specification) server
to allow external management tools to dynamically configure SPDK components.
In order to enhance the existing functions and optimize the interface display, 
the RPC of the LAVA project is added.

## lava.py
The current RPC interface is optimized in complete decoupling mode. A new lava
interface(lava.py) is designed to receive commands. The commands are processed
by the lava interface directly or forwarded to the original rpc interface for 
processing. The display mode of some query commands has been optimized to 
user-friendly tables.

Example:

~~~bash
scripts/lava.py bdev_ocf_get_stats CAS1
~~~

A brief description of each of the RPC methods and optional 'rpc.py' arguments c
an be viewed with:

~~~bash
scripts/lava.py --help
~~~

A detailed description of each RPC method and its parameters is also available. For example:

~~~bash
scripts/lava.py bdev_ocf_get_stats --help
~~~

### bdev_ocf_get_stats

Get statistics of chosen OCF block device.

#### parameters

Name                    | optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name

#### Response

statistics as json object.

### Example

Example Request:

~~~json
{
    "jsonrpc": "2.0",
    "method": "bdev_ocf_get_stats",
    "id": 1
}
~~~

Example response:

╔══════════════════╦══════════╦═══════╦═════════════╗
║ usage statistics ║    count ║     % ║ units       ║
╠══════════════════╬══════════╬═══════╬═════════════╣
║ occupancy        ║        8 ║   0.0 ║ 4KiB blocks ║
║ free             ║ 51874872 ║ 99.99 ║ 4KiB blocks ║
║ clean            ║        8 ║ 100.0 ║ 4KiB blocks ║
║ dirty            ║        0 ║   0.0 ║ 4KiB blocks ║
╚══════════════════╩══════════╩═══════╩═════════════╝

╔═════════════════════╦═══════╦═══════╦══════════╗
║ usage statistics    ║ count ║     % ║ units    ║
╠═════════════════════╬═══════╬═══════╬══════════╣
║ rd_hits             ║     1 ║ 33.33 ║ Requests ║
║ rd_partial_misses   ║     1 ║ 33.33 ║ Requests ║
║ rd_full_missed      ║     1 ║ 33.33 ║ Requests ║
║ rd_total            ║     3 ║ 100.0 ║ Requests ║
║ rd_pt               ║     0 ║   0.0 ║ Requests ║
╚═════════════════════╩═══════╩═══════╩══════════╝
║ wr_hits             ║     0 ║   0.0 ║ Requests ║
║ wr_partial_misses   ║     0 ║   0.0 ║ Requests ║
║ wr_full_missed      ║     0 ║   0.0 ║ Requests ║
║ wr_total            ║     0 ║   0.0 ║ Requests ║
║ wr_pt               ║     0 ║   0.0 ║ Requests ║
╚═════════════════════╩═══════╩═══════╩══════════╝
║ pf_partial_misses   ║     0 ║   0.0 ║ Requests ║
║ pf_full_misses      ║     0 ║   0.0 ║ Requests ║
║ pf_total            ║     0 ║   0.0 ║ Requests ║
║ pf_pt               ║     0 ║   0.0 ║ Requests ║
╚═════════════════════╩═══════╩═══════╩══════════╝
║ rd_total            ║     3 ║ 100.0 ║ Requests ║
║ rd_pt               ║     3 ║ 100.0 ║ Requests ║
╚═════════════════════╩═══════╩═══════╩══════════╝

╔════════════════════╦═══════╦═══════╦═════════════╗
║ block statistics   ║ count ║     % ║ units       ║
╠════════════════════╬═══════╬═══════╬═════════════╣
║ core_volume_rd     ║     9 ║ 100.0 ║ 4KiB blocks ║
║ core_volume_wr     ║     0 ║   0.0 ║ 4KiB blocks ║
║ core_volume_total  ║     9 ║ 100.0 ║ 4KiB blocks ║
╚════════════════════╩═══════╩═══════╩═════════════╝
║ cache_volume_rd    ║     1 ║  10.0 ║ 4KiB blocks ║
║ cache_volume_wr    ║     9 ║  90.0 ║ 4KiB blocks ║
║ cache_volume_total ║    10 ║ 100.0 ║ 4KiB blocks ║
╚════════════════════╩═══════╩═══════╩═════════════╝
║ volume_rd          ║    10 ║ 100.0 ║ 4KiB blocks ║
║ volume_wr          ║     0 ║   0.0 ║ 4KiB blocks ║
║ volume_total       ║    10 ║ 100.0 ║ 4KiB blocks ║
╚════════════════════╩═══════╩═══════╩═════════════╝
║ prefetch_total     ║     0 ║   0.0 ║ 4KiB blocks ║
╚════════════════════╩═══════╩═══════╩═════════════╝
║ das_limit_io_total ║     0 ║   0.0 ║ 4KiB blocks ║
╚════════════════════╩═══════╩═══════╩═════════════╝

╔═════════════════════╦═══════╦═══════╦══════════╗
║ debug_io statistics ║ count ║     % ║ units    ║
╠═════════════════════╬═══════╬═══════╬══════════╣
║ entry_rd            ║    57 ║ 100.0 ║ Requests ║
║ entry_wr            ║     0 ║   0.0 ║ Requests ║
╚═════════════════════╩═══════╩═══════╩══════════╝

╔════════════════════╦═══════╦═════╦══════════╗
║ errors statistics  ║ count ║   % ║ units    ║
╠════════════════════╬═══════╬═════╬══════════╣
║ core_volume_rd     ║     0 ║ 0.0 ║ Requests ║
║ core_volume_wr     ║     0 ║ 0.0 ║ Requests ║
║ core_volume_total  ║     0 ║ 0.0 ║ Requests ║
╚════════════════════╩═══════╩═════╩══════════╝
║ cache_volume_rd    ║     0 ║ 0.0 ║ Requests ║
║ cache_volume_wr    ║     0 ║ 0.0 ║ Requests ║
║ cache_volume_total ║     0 ║ 0.0 ║ Requests ║
╚════════════════════╩═══════╩═════╩══════════╝
║ total              ║     0 ║ 0.0 ║ Requests ║
╚════════════════════╩═══════╩═════╩══════════╝

### bdev_ocf_reset_stats

Reset statistics if chosen OCF block device.

#### Parameters

Name                    | optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name

#### Response

Completion status of reset statistics opreration returned as a boolean.

#### Example

Example request:

~~~json
{
    "param": {
        "name": "ocf0"
    },
    "jsonrpc": "2.0",
    "method": "bdev_ocf_reset_stats",
    "id": 1
}
~~~

Example response:

~~~json
{
    "jsonrpc": "2.0",
    "id": 1,
    "result": true
}
~~~

### bdev_get_iostat

Get I/O statistics of block device (bdevs).

#### Parameters

The user may specify no parameters in order to list all block devices, or a block device may be
specified by name.

Name                    | optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name

#### Response

The response is an array of objects containing I/O statistics of the requested block device/

#### Example

Example request:

~~~json
{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "bdev_get_iostat",
    "param": {
        "name": "NVMe1n1"
    }
}
~~~

Example Response:

 tick_rate  100000000
 ticks      190879123802493

╔════════════════════════════════════════╗
║             NVMe1n1                    ║
╠═════════════════════════╦══════════════╣
║ bytes_read              ║ 659410944    ║
║ num_read_ops            ║ 160975       ║
║ read_latency_ticks      ║ 5229961217   ║
║ read_latency_ticks_min  ║ 899          ║
║ read_latency_ticks_max  ║ 894320       ║
║ read_latency_ticks_avg  ║ 4096         ║
╚═════════════════════════╩══════════════╝
║ bytes_read              ║ 9397714944   ║
║ num_read_ops            ║ 1749771      ║
║ write_latency_ticks     ║ 205842213641 ║
║ write_latency_ticks_min ║ 736          ║
║ write_latency_ticks_max ║ 6469225      ║
║ write_latency_ticks_avg ║ 5370         ║
╚═════════════════════════╩══════════════╝
║ bytes_unmapped          ║ 0            ║
║ num_unmap_ops           ║ 0            ║
║ unmap_latency_ticks     ║ 0            ║
╚═════════════════════════╩══════════════╝

### bdev_reset_iostat

Reset I/O statistics of block device (bdevs).

#### Parameters

The user may specify no parameters in order to list all block devices, or a block device may be
specified by name.

Name                    | optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name

#### Response

Completion status of reset statistics opreration returned as a boolean.

#### Example

Example request:

~~~json
{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "bdev_reset_iostat",
    "param": {
        "name": "NVMe1n1"
    }
}
~~~

Example request:

~~~json
{
    "jsonrpc": "2.0",
    "id": 1,
    "result": true
}
~~~

### bdev_query_status

Query status of chosen bdev device or module.

#### parameters

Name                    | optional | Type        | Description
----------------------- | -------- | ----------- | -----------
name                    | Required | string      | Block device name

#### Response

Completion status of query peration returned as a boolean.

#### Example

Example request:

~~~json
{
    "param": {
        "name": "das"
    }
    "jsonrpc": "2.0",
    "method": "bdev_query_status",
    "id": 1
}
~~~

Example response:

~~~json
{
    "jsonrpc": "2.0",
    "id": 1,
    "result": true
}
~~~
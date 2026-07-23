# Workload traces

Input traces for `ftlsim --trace <file>`. One request per line:

```
W 1234     # write logical page 1234
R 1234     # read logical page 1234
1234       # bare LPN -- treated as a write
# comment  # blank lines and '#' lines are ignored
```

LPNs larger than the configured device are folded into the exported capacity
(`lpn % logical_pages`), so a trace can be replayed against any geometry.

| File | What it is |
|---|---|
| `sample.trace` | Hand-written, ~40 requests. Exercises every line form and shows repeated overwrites of one hot page. |
| `hotspot-2k.trace` | 2,000 writes, 90% of them landing in the first 10% of a 1,000-page address space. |

Run one with:

```bash
./build/ftlsim --trace testdata/workloads/hotspot-2k.trace --blocks 32 --pages-per-block 16
```

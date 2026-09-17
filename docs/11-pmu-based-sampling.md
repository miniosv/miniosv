## Sampling
Based on the Performance Measurement Unit

Periodically taking snapshots of a program's execution state is referred to as sampling. Sampling can provide deep insights into a program's behavior and performance, but it incurs considerable overhead, especially when sampling at high frequency and/or in virtualized environments. For further information regarding sampling overhead in miniOSv, consult the corresponding section of this documentation.

### Overview
MiniOSv leverages [PMU](./10-performance-monitoring.md) counter overflows to generate sampling interrupts, as they offer greater configurability and precision than timer interrupts. On each interrupt, a sampling routine runs, saving relevant parts of the programs execution state.
Our unikernel only implement the structure around sampling, so by design we do **not** provide a default sampling routine. Instead, the application is responsible for providing the interrupt handler that runs on every sample. 

### Interface
```c++
PerfSampler(uint64_t frequency, std::function handler, PMCEvent event)
```
- `frequency` takes the number of events (e.g. CPU cycles) between two samples
- `handler` takes a function that will be run on every sample
- `event` takes an event struct expressing which event should be counted (defaults to `PERF_COUNT_HW::CPU_CYCLES`)

> [!NOTE]
> The performance counter will be reset to the corresponding value (defined by the frequency) after each sample automatically, this does not have to be part of the handler function.

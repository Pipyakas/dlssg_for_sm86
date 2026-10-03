# Archived next-release plan

These were plans for the successor to 0.2.4, which remained D3D12 with the 310.1 model.

1. Vulkan integration and resource interoperability: image layouts, queue synchronization,
   per-generated-frame calls, reuse of the SM75/SM86 pipeline, and validation of On/Off,
   Reset, 2x/3x/4x, output correctness and GPU timing.
2. Migration to the latest available DLSSG model: pin its version and source hashes,
   review model graphs/operators, retain the self-contained DLL, and compare quality,
   memory, stability and performance on SM75/SM86 against the prior model and a common
   release baseline.

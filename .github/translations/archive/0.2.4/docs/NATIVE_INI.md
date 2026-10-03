# Native 0.2.4 configuration (archived)

The everyday configuration has five keys. Edit `dlssg_sm86.ini` beside the selected
proxy DLL, then restart the game.

```ini
[Compatibility]
Router=SM86
KernelImage=PTX
HardwareBilinear=0
[FrameGeneration]
MaxGeneratedFrames=3
[Logging]
Level=1
```

| Key | Default | Meaning |
|---|---|---|
| Router | SM86 | Use SM86 for Ampere, SM75 for Turing or SM75 routing tests. |
| KernelImage | PTX | Driver JIT; Auto uses Cubin on an exact physical GPU/router match, otherwise PTX. Cubin requires an exact match. |
| HardwareBilinear | 0 | Exact output; 1 enables approximate hardware sampling on SM86 only. |
| MaxGeneratedFrames | 3 | Capability limit: 1/2/3 generated frames corresponds to 2x/3x/4x; the game requests the actual count. |
| Level | 1 | 0 off, 1 errors, 2 diagnostics, 3 detailed logs. |

## Presets and fixed exact optimizations

`config/presets/sm86-default.ini` uses HardwareBilinear=0; the performance preset
uses 1. Preserve your router and multiplier cap before copying a preset. Approximate
sampling changes generated pixels; quality and speed depend on the input.

SM86's default variants, optimized convolutions, decoder/pair/merge, block-1 residual
chain, exact image patches, and CUDA uint32 clearing are fixed on. Clearing reuses
kernel 58, retaining original sentinel values and UAV barriers. ChainBlock0,
PlainVariant, DependencyBarriers, OptimizedKernels, DisableFusions, ImagePatches,
and CudaBufferClear are no longer runtime switches. Historical ablation settings
apply only to their corresponding historical DLL.

## SM75

Use Router=SM75 and KernelImage=PTX. Auto selects PTX when testing that route on a
physical SM86 GPU; Cubin explicitly rejects the mismatch. SM75 uses its own entire
kernel path, original D3D12 clearing and the shared wrapper, without SM86 fusions,
CUDA clearing or approximate sampling. HardwareBilinear=1 has no effect there.
Forward PTX was checked on a 3080 Ti; physical Turing/Cubin remained unverified.

## Optional diagnostics

```ini
[Diagnostics]
Performance=1
PipelineSteps=0
[Logging]
Level=2
EvaluateEvery=120
```

Modify the existing Logging section, rather than creating a duplicate. Logs default
to `dlssg_sm86/logs/native_<PID>.jsonl`. Performance enables asynchronous GPU timing;
PipelineSteps=1 enables per-step timestamps and affects performance. Remove the
Diagnostics section and restore Level=1 afterward. These measure the GPU pipeline,
not Reflex or display cadence.

Logging File/DebugOutput/Directory, Debug MarkGeneratedFrames/MarkerX/MarkerY/MarkerScale,
and General Enabled are still supported. Old runtime/cache/architecture-spoof settings
do not participate in this archived Native path.

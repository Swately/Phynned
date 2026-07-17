<!-- NOTE 2026-07-15: this is the PHYNNED INSTANCE (the [V1] lived source) of the general
method now formalized at protocols/quality/EMPIRICAL_TEST_PROTOCOL.md. This doc stays the
phynned-specific instrument (scenarios, PresentMon, bench runner, pitfalls); the general
protocol is what other projects instantiate. -->

# Phynned — Empirical Test Protocol

Procedimiento reproducible para validar empíricamente las policies de Phynned
contra un workload real. **Este es el work-flow oficial** que produce los
reportes de `docs/phynned/reports/`.

**Version:** 2.0 (post-UI bench runner + 9 validated reports).
**DoD relacionado:** `AYAMA_MASTER_PLAN.md §8.6` item #3.
**Evidencia acumulada:** ver `docs/phynned/reports/EMPIRICAL_EVIDENCE_SUMMARY.md`.

> **v2.0 changes**: 5-run A/B/A/B/A protocol (replaced 1-run A/B);
> UI bench runner (replaced PowerShell + manual PresentMon); IPC
> pause/resume commands (replaced kill-and-restart agent);
> methodological pitfalls section added (§7).

---

## §0 — Filosofía

> "Toda métrica tiene baseline. No se reporta una mejora sin haber medido
> el estado pre-Phynned." — Master Plan §0.4 regla #5.

Cada test ejecuta DOS fases sobre el **mismo escenario reproducible**:

- **Phase A (Baseline)**: Phynned corriendo pero **sin policies activas** (modo observe-only).
  Mide cómo se comporta el juego sin intervención.
- **Phase B (Treated)**: Phynned corriendo con la policy correspondiente al hardware.
  Mide el delta.

La diferencia entre A y B es el valor real de Phynned. Si el delta no es
estadísticamente significativo, se reporta como tal — honestidad antes que números.

---

## §1 — Pre-requisitos

### 1.1 Hardware

- Una CPU con perfil claro:
  - **AMD X3D** (5800X3D, 7800X3D, 7950X3D, 9800X3D) → policy `PinGameToVCacheCcd`
  - **Intel hybrid** (12700K+, 13700K+, 14700K+) → policy `PinGameToPCores`
  - **AMD multi-CCD sin V-Cache** → policy `IsolateGameFromBackground`
  - **Single-CCD homogénea** → control negativo (Phynned no actúa)
- RAM ≥ 16 GB, idealmente DDR5 para X3D / DDR4-3600+ para resto.
- GPU con headroom suficiente para que el juego sea **CPU-bound** (no GPU-bound).
  Si la GPU está al 100% no veremos beneficio de Phynned — el CPU ni se acerca al límite.
  **Antes de iniciar el A/B/A/B/A, verifica GPU% en RTSS/Afterburner según §7.3
  (objetivo 60-85% sostenido). El error metodológico #1 documentado en Reports 7
  vs 10 fue testear con GPU al 95% y declarar falso NULL.**

### 1.2 Software baseline

| Pieza | Versión recomendada | Notas |
|-------|---------------------|-------|
| Windows | 10 22H2 o 11 23H2+ | Más nuevo = más providers ETW |
| phynned-agent.exe | build actual del repo | Correr como Admin (UAC manual o servicio) |
| PresentMon | 2.x desde [github.com/GameTechDev/PresentMon](https://github.com/GameTechDev/PresentMon/releases) | Microsoft, gratis, < 5 MB. Captura frame times via ETW |
| phynned-cli.exe | build actual del repo | Para el `presentmon-import` + `bench diff` |
| El juego/workload | ver §2 | El que vas a testear |

### 1.3 Privilegios

- **phynned-agent.exe debe correr como Administrator** o cambios de affinity fallan
  con `PermissionDenied`. Verifica con:
  ```cmd
  phynned-cli status
  ```
  Si reporta `Privilege: None` → no es admin. Re-elevar.

- PresentMon también necesita admin (usa session ETW privada).

---

## §2 — Escenarios pre-aprobados

Ordenados por reproducibilidad (alto → bajo) y valor de demostración.

### 2.1 Minecraft Java + Distant Horizons + shaders (escenario inicial)

**Por qué:** Cero anti-cheat, easy admin, produce 1% lows extremos por chunk
loading + GC pauses + shader compile + LOD render distance. Phynned X3D puede
aliviar la presión sobre el thread principal.

**Setup mínimo (single-player, reproducible):**

```
Launcher:          Prism Launcher / Mojang Launcher / MultiMC
Edition:           Java 1.20.x o 1.21.x
Loader:            Fabric 0.15+
Mods:
  - Sodium 0.5+               (rendering rewriter; mandatory para perf)
  - Iris 1.7+                 (shader support)
  - Lithium 0.12+             (gameplay optimization)
  - Distant Horizons 2.x      (LOD distantes — clave del stutter test)
  - (opcional) FerriteCore + ImmediatelyFast
Shader:            Complementary Reimagined Unbound v5.x  (heavy, well-known)
                   o BSL Shaders extreme preset
Render distance:   Vanilla 12 chunks  +  DH 256 chunks
World:             Crear con seed FIJO (apunta el número)
                   "Amplified" world type para mucho terreno
```

**Test scene (idéntica entre runs):**

1. Spawn en el seed conocido.
2. Comando: `/gamemode creative` (sin daño/hambre interfiriendo).
3. Comando: `/tp ~ 200 ~` (volar alto, view distance lleno de chunks).
4. Vuelo continuo dirección norte a velocidad constante (`/effect give @s speed 60 5`).
5. Capturar 90 segundos (suficiente para 5000+ frames a 60 FPS).

### 2.2 Cyberpunk 2077 (escenario futuro, opcional)

**Mejor target** que Minecraft para repetibilidad:
- Tiene built-in benchmark (Settings → Graphics → Run benchmark).
- Conocido por beneficiar V-Cache (HW Unboxed / Gamers Nexus benchmarks).
- DLSS off, máximo CPU load.

Test scene: el benchmark oficial corre exactamente igual cada vez.

### 2.3 Otros candidatos sin anti-cheat

Hogwarts Legacy, Stalker 2, The Witcher 3 Next-Gen, God of War, Returnal, Spider-Man Remastered.

---

## §3 — Procedure step-by-step (v2.0 — UI bench runner)

Since v2.0 the entire flow is automated by the UI bench runner.
**The v1.0 protocol (PowerShell + manual PresentMon) is deprecated** — only use it if the UI is not working.

### 3.1 Pre-test

1. **Cierra todo lo que no necesites.** Discord, Steam overlay, browsers
   con muchas pestañas. Esto reduce ruido de fondo en los reads de
   PresentMon y deja CPU headroom claro para Phynned.

2. **Lanza el juego primero** y llega al escenario que vas a testear
   (§2 — escenarios pre-aprobados). Hazlo ANTES de arrancar el agent
   para que el classifier tenga el game ya en foreground.

3. **Lanza `phynned-agent.exe` como Administrator** (admin necesario para
   ETW + PROCESS_SET_INFORMATION):
   ```powershell
   # From an elevated PowerShell, from the repo root:
   & ".\build\apps\phynned\tools\phynned-agent\phynned-agent.exe" 2>&1 |
       Tee-Object -FilePath ".\agent_test.log"
   ```

4. **Verifica que el juego fue clasificado como Game.** En el log del
   agent debería aparecer:
   ```
   [Phynned][Classify] <gamename>.exe [pid=NNNN] → Game | fs=1 ...
   ```
   Si aparece como Unknown, revisa §7 (pitfalls).

5. **Lanza `phynned-ui.exe` como Administrator** (PresentMon hereda los
   privilegios via spawn). Tab "Benchmark".

### 3.2 Run the 5-run A/B/A/B/A protocol

En el bench panel:

1. **Quick-pick** del proceso del game (e.g. `HogwartsLegacy.exe`,
   `MCC-Win64-Shipping.exe`, `Borderlands2.exe`). Si auto-discovery
   añadió un PID launcher 1-thread + el game con N-threads, **selecciona
   el game** (más threads / más RAM).
2. **Duration**: 30 s default. Para tests "publication-grade" sube a
   60 o 90 s.
3. **Click "Run A/B/A/B/A protocol (5 runs)"**.
4. Espera ~3 minutos sin interferir. El runner orquesta:
   - 5 capturas de PresentMon × 30 s = 150 s
   - 4 cooldowns × 3 s = 12 s
   - 4 ack-waits para IPC pause/resume = ~400 ms total
   - 5 imports de phynned-cli presentmon-import
   - 1 agregación final phynned-cli bench multi
   - Total ~3 minutos + overhead I/O

5. **Lee el output** en el panel multi-line: incluye los 5 runs +
   verdict final. Copia para el reporte (§4).

### 3.3 Verificación post-test

- **Frame counts proporcionales**: si baseline ~5000 frames y treated
  ~7000 frames en runs iguales, es señal de mejora real. Si los counts
  son idénticos pero las stddev cambiaron, es variance reduction sin
  mean improvement (legítimo pero menos impactante).
- **Run 1 frecuentemente es outlier** (cold cache, shader compile). Si
  Run 1 baseline tiene FPS o variance muy distintos a Runs 3, 5,
  trátalo como warm-up y nota la observación en el reporte.
- **Treated stddev mucho menor** que baseline stddev es **la firma de
  Phynned working** — incluso si la media no se mueve mucho, el frame
  pacing se estabiliza.

### 3.4 Análisis

El bench runner ya corrió `phynned-cli bench multi` por ti. Si quieres
re-correrlo manualmente (e.g. excluyendo runs específicos):

```powershell
& ".\build\apps\phynned\tools\phynned-cli\phynned-cli.exe" bench multi `
    --baseline "$env:TEMP\phynned-bench\run1.bench.csv" `
    --baseline "$env:TEMP\phynned-bench\run3.bench.csv" `
    --baseline "$env:TEMP\phynned-bench\run5.bench.csv" `
    --treated  "$env:TEMP\phynned-bench\run2.bench.csv" `
    --treated  "$env:TEMP\phynned-bench\run4.bench.csv"
```

### 3.5 Cómo interpretar el verdict

| Verdict | Avg / P99 / P99.9 CIs | Significado |
|---|---|---|
| **SIGNIFICANT IMPROVEMENT (P99.9 CI strictly below baseline CI)** | Las 3 sin overlap | Phynned redujo frame times con statistical significance. Reportable. |
| **MARGINAL IMPROVEMENT (P99 significant, P99.9 trending but CI overlap)** | Avg/P99 sin overlap, P99.9 overlap | Mejora real pero limitada por sample size en deep tail. Reportable. |
| **NO STATISTICALLY DETECTABLE DIFFERENCE (CIs overlap)** | Todos overlap | Null result. Honesto reportarlo. Posibles causas: workload no CPU-bound, engine bien threaded, VSync cap, GPU-bound, anti-cheat blocking. Ver §7. |
| **REGRESSION** | Treated CI strictly above baseline CI | Phynned empeoró el resultado. Investigar — posible bug, mala policy match para el CPU, contaminación de cache de Phynned. |

**Caveat sobre P99.9 vs max frame time**: el bench multi tool usa P99.9
como driver del verdict. Pero con runs de 30 s × 470+ FPS = 14,000+
frames, P99.9 = los 14 frames peores. Spikes raros pero severos (cross-
CCD migration events) NO mueven mucho P99.9 porque sus 14 worst frames
ya incluyen el "natural worst case" no-spike. **Max frame time** SÍ
captura esos spikes (e.g. baseline max 14ms vs treated max 6ms — vista
en Borderlands 2 test). En reportes, mira el `max_ms` de los CSVs
individuales junto al verdict.

---

## §4 — Reporte format (para `docs/phynned/reports/`)

Crear un archivo: `docs/phynned/reports/minecraft-dh-shaders_<date>_<cpu>.md`

```markdown
# Scenario: Minecraft Java + Distant Horizons + shaders

## Hardware

- CPU: AMD Ryzen 7 7800X3D (single CCD, V-Cache on cores 0-7)
- RAM: 32 GB DDR5-6000 CL30
- GPU: RTX 4080
- Storage: NVMe Gen4
- OS: Windows 11 23H2

## Software

- Phynned: build commit XXXX
- Minecraft: Java 1.21.x
- Mods: Sodium 0.5.11, Iris 1.7.3, Lithium 0.12.7, Distant Horizons 2.1.1
- Shader: Complementary Reimagined Unbound r5.2
- DH render distance: 256 chunks
- Vanilla render distance: 12 chunks

## Workload

- Seed: 1234567890
- World: Amplified, peaceful
- Test scene: creative + speed-60 + vuelo norte 90 s
- Captura: PresentMon 2.x, 90 s timed

## Phase A — Baseline (Phynned observe-only)

[Pegar output del `presentmon-import baseline`]

## Phase B — Treated (Phynned PinGameToVCacheCcd, mask=0x000000FF)

[Pegar output del `presentmon-import treated`]

## Delta

[Pegar output del `bench diff`]

## Verdict

[X% improvement on P99 / Marginal / No measurable change]

## Notes / Observations

- (¿Hubo shader recompile durante alguna fase? Notarlo)
- (¿GC pauses visibles en Java? Notarlo)
- (¿Otra app activa que afectó? Notarlo)
```

---

## §5 — Edge cases y troubleshooting

| Síntoma | Causa probable | Fix |
|---------|----------------|-----|
| `PresentMon` no captura nada | Necesita admin | Right-click → Run as Administrator |
| `phynned-cli targets` no muestra javaw | El observer no detecta el proceso por nombre | El default policy pack lista patrones — añadir `javaw.exe` en `policies.toml` |
| `phynned-cli actions` muestra `failed: PermissionDenied` | Agent no es admin | Reelevar UAC |
| Verdict: NO CHANGE pero esperabas mejora | Workload era GPU-bound | Bajar settings gráficos para forzar CPU-bound; verificar GPU% durante test |
| Verdict: REGRESSION | Tu CPU es single-CCD o Phynned mis-clasificó | `phynned-cli memory list` + considerar `memory clear` |
| P99 mejora pero variance no | Stutters específicos (e.g. shader compile) no afectados por affinity | Esperado — Phynned no resuelve TODOS los stutters |

---

## §6 — Próximos pasos (después del primer reporte)

1. **Si el verdict es SIGNIFICANT**: replicar en al menos 2 CPUs distintos
   (per DoD §8.6 item #2).
2. **Si el verdict es NO CHANGE / REGRESSION**: investigar por qué — añadir
   diagnostic logging al MetricsCollector + revisar la policy aplicada.
3. **Capturar más escenarios** según §2.3 (Cyberpunk built-in benchmark es
   el siguiente más reproducible).
4. **Fase B del Master Plan §11.4**: implementar captura interna de frame
   times (DxgKrnl provider) para eliminar PresentMon como dependencia
   externa. El protocol se reduce a un solo botón en `phynned-ui`.

---

---

## §7 — Methodological pitfalls (lessons learned from 9 tests)

Insights from `EMPIRICAL_EVIDENCE_SUMMARY.md`. Ignorar estos pitfalls
produce reports inválidos o engañosos.

### 7.1 — Cold-cache outlier on Run 1

**Síntoma**: Run 1 baseline tiene avg/max ms significantly higher que
runs 3, 5. Run 1 puede tener un spike de 30-100 ms en `max_ms` mientras
los otros baselines son <10 ms.

**Causa**: Primera vez que el game compila shaders / abre archivos /
poblar caches después de relanzar. ~5-30 segundos de warm-up.

**Mitigación**:
- Da 30+ segundos de warm-up al game antes de iniciar el A/B/A/B/A.
- Si Run 1 sigue siendo outlier, NOTAR en el reporte que Avg/P99/P99.9
  son robustos (single spike no mueve mucho la mediana), pero
  `stutter_abs_60hz` de Run 1 puede estar inflado.

### 7.2 — VSync / framerate cap masking the result

**Síntoma**: Avg ms idéntico entre baseline y treated. Treated **no
gana FPS** porque ambos ya están en el techo del display refresh rate.

**Causa**: Si tu pantalla es 144 Hz y el game va a 144 FPS (VSync ON),
Phynned no puede subir FPS — el bottleneck es el monitor, no el CPU.

**Mitigación**:
- **Disable VSync** en el game.
- **Disable engine-level caps** (e.g. Bethesda games tienen
  `bSmoothFrameRate=True` en .ini files).
- Si el game tiene physics-FPS coupling (Bethesda Creation Engine),
  reconoce que el VSync es NECESARIO para jugabilidad — el resultado
  será conservador (ej: Fallout 4 +35% es conservador; sin physics
  constraint sería ~+50-60%).

### 7.3 — GPU-bound workload (the most common false-NULL cause)

**Síntoma**: GPU al 95-100% durante toda la captura. Frame times muy
consistentes (low variance), no hay spikes. Treated ≈ baseline.

**Causa**: GPU es el bottleneck, no el CPU. Phynned no acelera GPU.

**Caso de estudio (Mayo 2026)**: Reports 7 vs 10 son el mismo juego
(RDR2), misma máquina, misma build de Phynned, **pero distintas escenas**:

| Test | Scene | Render scale | GPU util | Resultado |
|---|---|---|---|---|
| Report 7 (FALSE NULL) | Free-roam horizon | Alta | **95%** | NULL within CI |
| Report 10 (TRUE POSITIVE) | Saint Denis night | Default | **84% / 88%** | **+13.3% SIG** |

Report 7 clasificó RDR2 como NULL durante 6 meses, hasta que se
descubrió que la escena de medición estaba GPU-bound. Misma config CPU,
misma Rule 1, misma metodología — pero la GPU saturada enmascaraba
totalmente la mejora.

**Diagnóstico operacional ANTES de correr el A/B/A/B/A**:

1. Lanzar el juego, posicionarse en la escena que se va a benchmarkear.
2. Abrir RTSS / MSI Afterburner / GPU-Z. Mirar GPU% sostenido durante 30s.
3. Aplicar el siguiente árbol de decisión:

| GPU% sostenido | Acción |
|---|---|
| **<60%** | CPU-bound claro. Proceder con el bench. |
| **60-85%** | CPU-bound dominante con margen GPU. Proceder. Sweet spot. |
| **85-95%** | Marginal. Considerar bajar settings antes del bench. |
| **>95%** | **GPU-bound. NO benchmarkear esta escena**. Bajar settings o cambiar escena. |

4. Mientras corre el bench, también monitorea ΔGPU% entre baseline
   (policies paused) y treated (Phynned active). **Si Phynned reduce CPU
   latency, GPU% sube algunos puntos** (ej. 84% → 88% en Report 10).
   Si ΔGPU% ≈ 0, estás GPU-bound aunque no llegues al 95% — el CPU no
   era el cuello de botella.

**Mitigación si GPU-bound**:
- **Lower graphics settings** para forzar CPU-bound (lower res, lower
  textures, lower draw distance, ray tracing OFF, DLSS Performance).
- **Choose CPU-heavy scenes** (combate intenso, mucho NPC streaming,
  vehicle physics, dense urban).
- **Si el game es inherentemente GPU-bound en este hardware**, reportar
  null honestamente — no es failure de Phynned, es scenario mismatch.
  Pero antes de aceptar el null, intenta una escena CPU-heavy del mismo
  juego: el resultado puede invertirse dramáticamente (Report 7 → 10).

### 7.4 — Modern engine ≠ guaranteed NULL (revised Mayo 2026)

**Versión anterior (incorrecta)**: "Engines well-threaded no se benefician".

**Versión correcta**: Engines well-threaded **se benefician menos**,
pero solo NULL si encima están GPU-bound. RDR2 Saint Denis night
(Report 10) demuestra que RAGE engine — el ejemplo canónico previo de
"well-threaded NULL" — produce **+13.3% SIGNIFICANT** cuando el escenario
satura el CPU.

**Síntoma del null genuino bajo este modelo**:
- Engine well-threaded (RAGE, modded Java, UE5 task-graph), Y
- Workload con muchos threads activos simultáneamente (verificable
  con `[Phynned][HotThread]` log o Process Explorer), Y
- GPU-bound (ver §7.3 para el diagnóstico)

Cualquiera de las tres condiciones que falte → el NULL es probablemente
falso negativo. Re-test en escena CPU-heavy antes de aceptar.

**Caso de estudio**: Report 7 vs 10 (RDR2) — la diferencia fue 100%
metodológica (escena GPU-bound vs CPU-bound), 0% engine. RAGE no es
inmune al pinning de V-Cache: solo lo necesita en escenas donde el CPU
sea el cuello de botella.

**Mitigación**: Antes de reportar NULL en un engine "moderno", verificar
que se cumplieron las tres condiciones de arriba. Si dudas, mover la
cámara a una escena con más NPCs / scripting / draw calls y re-correr.

### 7.5 — Game has two processes with same exe name (launcher + game)

**Síntoma**: Game classification stuck en Unknown a pesar de signals
correctos. El log muestra `threads=1` para el game cuando esperaríamos
30-50+.

**Causa**: Game ships with a launcher stub + the real game process,
ambos llamados igual (e.g. `HogwartsLegacy.exe` launcher PID + real
game PID). Cache colliding by name.

**Mitigación**: Confirmar con `Get-Process` que hay dos procesos. Bug
26 (PID-keyed cache) ya fixea esto post-Mayo-2026. Si ves la falla con
versión anterior, update Phynned.

### 7.6 — Legacy 32-bit games not detected

**Síntoma**: Game es viejo (pre-2015), `check_d3d_vk_modules` retorna
false aunque el game claramente usa DirectX. Game no clasifica como
Game.

**Causa**: `EnumProcessModules` (sin Ex) falla cuando un caller 64-bit
consulta un target 32-bit. Bug 29 fijo en Mayo 2026 con
`EnumProcessModulesEx(LIST_MODULES_ALL)`.

**Mitigación**: Update Phynned. Si Phynned está actualizado pero el game
sigue sin detectar, puede ser nombre de exe no en seed patterns —
añadir manualmente o esperar auto-discovery (~30s en foreground).

### 7.7 — Steam UI / Discord / Electron apps registered as Game

**Síntoma**: La UI muestra Steam UI (steamwebhelper.exe) o Discord
como Kind=Game. Falso positivo.

**Causa**: Estos apps usan CEF/Chromium con custom title bar (sin
WS_CAPTION). Sin el style check correcto, pasan el fullscreen gate.

**Mitigación**: Bug fix Mayo 2026 añade window class check
(`Chrome_WidgetWin_*`) + WS_THICKFRAME check. Update Phynned.

### 7.8 — Anti-cheat games are out of scope

**Síntoma**: Game con kernel-mode anti-cheat (LoL/Vanguard, Valorant,
Fortnite/EAC, Apex/EAC, CoD/Ricochet, BF6/EA-Javelin) puede:
- Negar `PROCESS_VM_READ` a Phynned → check_d3d_vk_modules falla
- Negar `PROCESS_SET_INFORMATION` → policy apply falla
- En peor caso: flagear Phynned como cheat tool → ban risk

**Mitigación**: **NO TESTEAR** estos juegos. Lista explícita de games
seguros para test está en `EMPIRICAL_EVIDENCE_SUMMARY.md §Scope`.

### 7.9 — Frame counts identical but stddev reduced

**Síntoma**: Avg FPS no cambia (e.g. +1-2% within CI), pero treated
stddev es 90%+ menor que baseline stddev.

**Causa**: Phynned está estabilizando frame pacing sin acelerar el frame
rate medio. Common en games GPU-bound o moderately threaded
(Hogwarts, RDR2, BL2).

**Mitigación**: Reportar como "variance reduction without mean
improvement" — es legítimo y útil para usuarios que ven jitter pero
no diferencia de benchmark. Documentar treated stddev junto al
verdict tradicional.

### 7.10 — Test scenario unreproducible

**Síntoma**: Repites el mismo test 3 días después y el resultado
cambia significativamente.

**Causa**: Scenario subjetivo ("look at a wall", "fight in this area")
no es reproducible. Posición del jugador, NPCs visibles, weather
state, time-of-day del game pueden variar.

**Mitigación**:
- Usa **save states** del game si lo soporta.
- Documenta **coordenadas exactas** del personaje.
- Captura un **screenshot** del scenario inicial.
- Si el game tiene **built-in benchmark** (Cyberpunk 2077, RDR2,
  Hitman, F1), úsalo en vez de gameplay manual — perfecto repetible.

---

**Fin del Empirical Test Protocol v2.0**
*Compañero de `AYAMA_MASTER_PLAN.md §5.4` (test methodology) + §7.2 (test scenarios) + §11.4 (oportunidades de mejora).*
*Reportes consolidados: `docs/phynned/reports/EMPIRICAL_EVIDENCE_SUMMARY.md`.*

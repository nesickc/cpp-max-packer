import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { Assets, Desktop, Results } from './generated/contracts';
import { bridge, nativeAvailable } from './bridge';
import { acceptState, activeOperation, defaultSettings, settingsError, snapshotSettings } from './state';
import { Viewer } from './Viewer';

const emptyPoses: Results.Placement[] = [];
function errorMessage(error: unknown): string {
  if (typeof error === 'object' && error && 'message' in error) return String(error.message);
  return String(error);
}
function failureFrom(details: unknown): Results.Failure | undefined {
  if (!details || typeof details !== 'object' || !('failure' in details)) return;
  const failure = details.failure;
  if (!failure || typeof failure !== 'object' || !('phase' in failure) || !('cause_code' in failure)) return;
  if (typeof failure.phase !== 'string' || typeof failure.cause_code !== 'string') return;
  const result: Results.Failure = { phase: failure.phase, cause_code: failure.cause_code };
  if ('resource' in failure && failure.resource && typeof failure.resource === 'object') {
    const resource = failure.resource;
    if ('name' in resource && typeof resource.name === 'string' && 'required' in resource && typeof resource.required === 'string' && 'limit' in resource && typeof resource.limit === 'string') {
      result.resource = { name: resource.name, required: resource.required, limit: resource.limit };
    }
  }
  if ('suggested_pitch_mm' in failure && typeof failure.suggested_pitch_mm === 'number' && Number.isFinite(failure.suggested_pitch_mm) && failure.suggested_pitch_mm > 0) result.suggested_pitch_mm = failure.suggested_pitch_mm;
  return result;
}
function FailureDetail({ failure, details }: { failure?: Results.Failure; details: unknown }) {
  return <>{failure && <span> {failure.phase}: {failure.cause_code}.{failure.resource && <> {failure.resource.name}: required {failure.resource.required}, limit {failure.resource.limit}.</>}{failure.suggested_pitch_mm && <> Try a voxel pitch of {failure.suggested_pitch_mm} mm and Start again.</>}</span>}<details><summary>Diagnostic details</summary><pre>{JSON.stringify(details, null, 2)}</pre></details></>;
}
function NumberField({ label, value, onChange, min = 0, suffix = 'mm' }: { label: string; value: number; onChange: (v: number) => void; min?: number; suffix?: string }) {
  return <label className="number-field"><span>{label}</span><div><input aria-label={label} type="number" min={min} step="any" value={Number.isFinite(value) ? value : ''} onChange={e => onChange(e.target.value === '' ? NaN : Number(e.target.value))} /><span>{suffix}</span></div></label>;
}

function RepairComparison({proposal}: {proposal: NonNullable<Assets.RepairProposal>}) {
  const before=proposal.before_diagnostics?.import, after=proposal.after_diagnostics?.import;
  const dimensions=(d: typeof before) => d?.mesh_bounds_mm ? d.mesh_bounds_mm.max.map((v,i)=>(v-d.mesh_bounds_mm!.min[i]).toFixed(4)).join(' × ') : 'Unknown';
  const cavities=(d: typeof before) => d?.containment_check==='complete' ? d.shells.filter(s=>s.depth!==null && s.depth!==undefined && s.depth%2===1).length : 'Unknown';
  return <div className="repair-comparison"><strong>{proposal.algorithm==='envelope'?'Packing envelope':'Candidate'}: {proposal.candidate_status}</strong>
    <table><thead><tr><th>Measurement</th><th>Before</th><th>After</th></tr></thead><tbody>
      <tr><td>Bounds (mm)</td><td>{dimensions(before)}</td><td>{dimensions(after)}</td></tr>
      <tr><td>Surface (mm²)</td><td>{proposal.surface_area_before_mm2?.toFixed(3)}</td><td>{proposal.surface_area_after_mm2?.toFixed(3)}</td></tr>
      <tr><td>Triangles</td><td>{before?.triangle_count}</td><td>{after?.triangle_count}</td></tr>
      <tr><td>Surface components</td><td>{before?.component_count}</td><td>{after?.component_count}</td></tr>
      <tr><td>Cavity shells</td><td>{cavities(before)}</td><td>{cavities(after)}</td></tr>
      <tr><td>Volume (mm³)</td><td>{before?.volume_mm3?.toFixed(3) ?? 'Unknown'}</td><td>{after?.volume_mm3?.toFixed(3) ?? 'Unknown'}</td></tr>
    </tbody></table>
    {proposal.algorithm==='envelope' ? <p>The original model is retained. A valid envelope independently encloses every retained source triangle. Recesses and cavities may be closed, reducing nesting.</p> : <p>No certified surface-deviation bound or sampled surface-deviation measurement is available.{proposal.max_displacement_mm!=null && <> Maximum mapped vertex displacement: {proposal.max_displacement_mm.toPrecision(5)} mm; this is not a surface bound.</>}</p>}
    {after?.issues.slice(0,5).map((issue,i)=><p key={i}>{issue.reason}: {issue.message}</p>)}
    {proposal.candidate_status!=='valid' && <p>Acceptance is blocked. Remaining invalid or indeterminate geometry needs further repair; changing packing pitch cannot resolve it.</p>}
  </div>;
}

export function App() {
  const native = nativeAvailable();
  const [state, setState] = useState<Desktop.State | null>(null);
  const [settings, storeSettings] = useState<Desktop.Settings>(() => structuredClone(defaultSettings));
  const edited = useRef(false);
  const setSettings: typeof storeSettings = value => { edited.current = true; storeSettings(value); };
  const [units, setUnits] = useState<Desktop.ImportRequest['units']>('mm');
  const [scale, setScale] = useState(1);
  const [repairAlgorithm, setRepairAlgorithm] = useState<'weld'|'manifold'>('manifold');
  const [repairTolerance, setRepairTolerance] = useState(1);
  const [repairView, setRepairView] = useState<'before'|'after'>('after');
  const [error, setError] = useState<unknown>(null);
  const [commandPending, setCommandPending] = useState(false);
  const [hidden, setHidden] = useState<ReadonlySet<string>>(new Set());
  const [selected, setSelected] = useState<string | null>(null);
  const [now, setNow] = useState(Date.now());
  const current = useRef<Desktop.State | null>(null);
  const generation = useRef(0);
  const opened = useRef<string | null>(null);
  const updateState = useCallback((incoming: Desktop.State) => {
    const accepted = acceptState(current.current, incoming);
    if (accepted === current.current) return;
    const first = !current.current;
    current.current = accepted; setState(accepted);
    const op = accepted.operation;
    if (((first && !edited.current) || (op?.kind === 'open' && op.phase === 'finished' && opened.current !== op.id)) && accepted.draft_settings) {
      storeSettings({ ...structuredClone(accepted.draft_settings), orientation: { mode: 'cube45' } }); if (op?.kind === 'open') { opened.current = op.id; edited.current = false; }
    }
  }, []);
  useEffect(() => {
    if (!native) return;
    let alive = true, inFlight = false;
    const poll = async () => {
      if (inFlight) return; inFlight = true; const requestGeneration = generation.current;
      try { const next = await bridge.state(); if (alive && requestGeneration === generation.current) updateState(next); }
      catch (e) { if (alive) setError(e); }
      finally { inFlight = false; }
    };
    void poll(); const timer = window.setInterval(poll, 250);
    return () => { alive = false; window.clearInterval(timer); };
  }, [native, updateState]);
  useEffect(() => { const timer = window.setInterval(() => setNow(Date.now()), 250); return () => window.clearInterval(timer); }, []);
  useEffect(() => { setHidden(new Set()); setSelected(null); }, [state?.object?.id]);
  const run = async (action: () => Promise<unknown>) => {
    setCommandPending(true); setError(null);
    try { await action(); updateState(await bridge.state()); } catch (e) { setError(e); }
    finally { setCommandPending(false); }
  };
  const newProject = () => run(async () => {
    generation.current++; const fresh = await bridge.newProject();
    current.current = null; opened.current = null; edited.current = false; updateState(fresh);
    storeSettings({ ...structuredClone(fresh.draft_settings ?? defaultSettings), orientation: { mode: 'cube45' } });
  });
  const busy = activeOperation(state), disabled = !native || commandPending || busy;
  const accepted = state?.object?.report.state === 'accepted' && state.object.report.diagnostics.status === 'valid';
  const validationError = settingsError(settings);
  const result = state?.result ?? null;
  const live = state?.live_result ?? null;
  const poseKey = live ? `${live.session_id}/${live.native_epoch}/${live.operation_id}/${live.settings_id}/${live.solution_revision}` : `${result?.id}/${result?.document.solution_revision}`;
  const resultSettings = useMemo(() => live?.settings ?? (result ? snapshotSettings(result) : null), [poseKey]);
  const pending = !!resultSettings && JSON.stringify(settings) !== JSON.stringify(resultSettings);
  const operation = state?.operation;
  const elapsed = operation?.completion_elapsed_seconds ?? (operation ? Math.max(0, ((operation.finished_at ? Date.parse(operation.finished_at) : now) - Date.parse(operation.started_at)) / 1000) : 0);
  const supportedThreads = state?.cpu_runtime?.supported_max ?? 1;
  const resource = state?.cpu_runtime?.resource_status;
  const poses = live?.placements ?? result?.document.placements ?? emptyPoses;
  const repair = state?.repair;
  const preview = repair ? (repair.algorithm==='envelope' ? repair.before : repair[repairView]) : state?.object?.report.packing_envelope ? state.object.preview : live ? live.preview : result ? result.preview : state?.object?.preview ?? null;
  const envelopePreview = repair ? (repair.algorithm==='envelope' ? repair.after : null) : state?.object?.envelope_preview ?? null;
  const displayedDimensions = useMemo(() => resultSettings?.box_dimensions_mm ?? (settings.box_dimensions_mm.every(v => Number.isFinite(v) && v > 0) ? settings.box_dimensions_mm : defaultSettings.box_dimensions_mm), [resultSettings, settings.box_dimensions_mm]);
  const report = state?.object?.report;
  // Native polling clones unchanged snapshots. Their revision and physical
  // dimensions, rather than transport object identities, own display updates.
  const displayPoses = useMemo<Results.Placement[]>(() => !repair && live ? live.placements : !repair && result ? result.document.placements : state?.object?.preview ? [{ copy_id: 'object-preview', translation_mm: displayedDimensions.map(v => v / 2) as [number,number,number], quaternion_xyzw: [0,0,0,1], local_to_world: [[1,0,0,displayedDimensions[0]/2],[0,1,0,displayedDimensions[1]/2],[0,0,1,displayedDimensions[2]/2],[0,0,0,1]] }] : emptyPoses, [poseKey, repair?.token, state?.object?.preview?.preview_id, displayedDimensions[0], displayedDimensions[1], displayedDimensions[2]]);
  useEffect(() => {
    const ids = new Set(displayPoses.map(p => p.copy_id));
    setSelected(old => old && ids.has(old) ? old : null);
    setHidden(old => [...old].every(id => ids.has(id)) ? old : new Set([...old].filter(id => ids.has(id))));
  }, [displayPoses]);
  const selectedPose = poses.find(p => p.copy_id === selected);
  const setDimension = (index: number, value: number) => setSettings(s => { const dimensions: [number, number, number] = [...s.box_dimensions_mm]; dimensions[index] = value; return { ...s, box_dimensions_mm: dimensions }; });
  const importObject = () => run(() => bridge.importObject(units === 'custom' ? { units, scale_mm: scale } : { units }));
  const startPacking = () => run(async () => {
    const next: Desktop.Settings = { ...structuredClone(settings), budget_scope: 'total_start' };
    await bridge.start({ settings: next });
    setSettings(next);
  });

  const errorDetails = error && typeof error === 'object' && 'details' in error ? error.details : state?.last_error?.details;
  const retainedFailure = state?.result?.document.search.diagnostics?.failure;
  const failure = failureFrom(errorDetails) ?? retainedFailure;
  return <div className="app-shell">
    <header className="app-header"><div className="brand"><span className="brand-mark">S</span><div>Spectra<span>Pack</span><small>SPACE, WELL USED.</small></div></div><div className="header-center"><span className="status-dot" />Desktop workspace </div><nav aria-label="Project actions"><button disabled={disabled} onClick={newProject}>New</button><button disabled={disabled} onClick={() => void run(() => bridge.open())}>Open project</button><button disabled={disabled || !accepted || !!validationError || (!!live && !result)} onClick={() => void run(() => bridge.save({ settings: structuredClone(settings) }))}>Save project</button></nav></header>
    {!native && <div className="preview-banner" role="status"><span>Browser preview</span> Launch the SpectraPack desktop app to import files, run CPU packing, and save projects.</div>}
    {(!!error || state?.last_error || failure) && <div className="error-banner" role="alert"><strong>{state?.last_error?.code ?? 'Run diagnostics'}</strong> {error ? errorMessage(error) : state?.last_error?.message ?? 'The valid result was retained after a search failure.'}<FailureDetail failure={failure} details={errorDetails ?? { failure }} />{!!error && <button aria-label="Dismiss operation error" onClick={() => setError(null)}>×</button>}</div>}
    <main className="workspace">
      <aside className="configuration" aria-label="Packing configuration"><div className="config-scroll"><div className="panel-heading"><span className="eyebrow">SET UP YOUR PACKING</span><h1>Packing setup</h1><p>Import an object and configure a box.</p></div>
        <section className="config-section"><div className="section-title"><span className="step">01</span><h2>Object</h2>{accepted && <span className="pill valid">Accepted</span>}</div>
          <div className="import-card"><span className="file-icon">◇</span><strong>{state?.object?.display_name ?? 'Select an object'}</strong><p>{report ? `${report.dimensions_mm.join(' × ')} mm` : 'Import creates a checked packing envelope and retains the original STL.'}</p><button className="secondary" disabled={disabled || (units === 'custom' && (!Number.isFinite(scale) || scale <= 0))} onClick={importObject}>{report ? 'Replace STL' : 'Import STL'} <span>↗</span></button></div>
          <p className="hint">Packing uses an outer envelope. Closed recesses can reduce nesting; inspect Original, Envelope or Overlay after import.</p>
          <label className="select-label">Source units<select aria-label="Source units" value={units} onChange={e => setUnits(e.target.value as typeof units)}><option value="mm">Millimeters (mm)</option><option value="inch">Inches (in)</option><option value="custom">Custom scale</option></select></label>
          {units === 'custom' && <NumberField label="Millimeters per source unit" value={scale} min={0.000001} onChange={setScale} />}
          {report && <div className="diagnostics"><strong>{report.packing_envelope ? 'Packing envelope validated' : report.diagnostics.status === 'valid' ? 'Geometry validated' : `Geometry ${report.diagnostics.status}`}</strong><p>{report.accepted_solid ? `${report.accepted_solid.triangle_count.toLocaleString()} ${report.packing_envelope ? 'packing-envelope' : 'accepted'} triangles` : 'No accepted solid available. Start is unavailable.'}</p>{report.packing_envelope ? <p>Original model retained. Packing uses an independently checked outer envelope; closed recesses may reduce nesting. Purple shows the envelope in Overlay view.</p> : report.repair_record && <p>Repair explicitly accepted. Original STL and repair provenance retained.</p>}{report.diagnostics.messages.map((message, i) => <p key={i}>{message}</p>)}
            {report.diagnostics.import && <><p>{report.diagnostics.import.boundary_edges} boundary edges · {report.diagnostics.import.nonmanifold_edges} non-manifold edges · {report.diagnostics.import.nonmanifold_vertices} non-manifold vertices</p><p>Intersection check: {report.diagnostics.import.intersection_check}; containment: {report.diagnostics.import.containment_check}.</p><details><summary>Geometry issues</summary>{report.diagnostics.import.issues.map((issue,i)=><p key={i}>{issue.reason}: {issue.message}</p>)}</details></>}
          </div>}
          {report && <details className="repair-panel" open={!accepted}><summary>Prepare for packing</summary>
            <p>Create an outer packing envelope automatically. The original model stays unchanged. Small recesses may be closed, leaving extra space between copies.</p>
            <button disabled={disabled} onClick={()=>void run(()=>bridge.repair({action:'envelope'}))}>Create packing envelope</button>
            <details><summary>Advanced geometry repair</summary>
            <p>These operations change the exported geometry. Use an envelope above to retain the original model.</p>
            <label className="select-label">Repair method<select aria-label="Repair method" value={repairAlgorithm} onChange={e=>setRepairAlgorithm(e.target.value as typeof repairAlgorithm)}><option value="manifold">Closed-surface reconstruction</option><option value="weld">Bounded vertex welding</option></select></label>
            <NumberField label={repairAlgorithm==='manifold'?'Repair resolution':'Weld tolerance'} value={repairTolerance} min={0.000001} onChange={setRepairTolerance}/>
            <p className="hint">Independent of voxel pitch. Reconstruction uses even/odd material sampling. Features, separate components and cavities below this resolution can disappear. It cannot infer missing walls or prove the intended shape. Inspect all changes before accepting.</p>
            <button disabled={disabled || !Number.isFinite(repairTolerance) || repairTolerance<=0} onClick={()=>void run(()=>bridge.repair({action:'propose',algorithm:repairAlgorithm,tolerance_mm:repairTolerance}))}>Propose repair</button></details>
            {repair && <><RepairComparison proposal={repair.report.repair_proposal!}/>
              {repair.algorithm!=='envelope' && <label className="select-label">Preview geometry<select aria-label="Preview geometry" value={repairView} onChange={e=>setRepairView(e.target.value as typeof repairView)}><option value="before">Original inspected mesh</option><option value="after">Repair candidate (not accepted)</option></select></label>}
              <button disabled={disabled || repair.report.repair_proposal?.candidate_status!=='valid'} onClick={()=>void run(()=>bridge.repair({action:'accept',token:repair.token}))}>{repair.algorithm==='envelope'?'Use packing envelope':'Accept repaired geometry'}</button>
              <button disabled={disabled} onClick={()=>void run(()=>bridge.repair({action:'reject',token:repair.token}))}>{repair.algorithm==='envelope'?'Discard envelope':'Reject repair'}</button>
            </>}
          </details>}
        </section>
        <section className="config-section"><div className="section-title"><span className="step">02</span><h2>Container</h2><span className="pill">Box interior</span></div><div className="dimension-fields">{['Width', 'Depth', 'Height'].map((label, i) => <NumberField key={label} label={label} value={settings.box_dimensions_mm[i]} min={0.000001} onChange={v => setDimension(i, v)} />)}</div><p className="hint">Internal dimensions · millimeters · Z up</p></section>
        <section className="config-section"><div className="section-title"><span className="step">03</span><h2>Clearance & orientation</h2></div><div className="two-fields"><NumberField label="Between copies" value={settings.clearance_mm.pair} onChange={v => setSettings(s => ({ ...s, clearance_mm: { ...s.clearance_mm, pair: v } }))} /><NumberField label="To walls" value={settings.clearance_mm.wall} onChange={v => setSettings(s => ({ ...s, clearance_mm: { ...s.clearance_mm, wall: v } }))} /></div><p className="hint">Allowed rotations: 96 orientations, including 45° turns around X, Y and Z.</p></section>
        <section className="config-section"><div className="section-title"><span className="step">04</span><h2>Search</h2><span className="pill cpu">CPU · {settings.thread_count ?? 1} thread{(settings.thread_count ?? 1) === 1 ? '' : 's'}</span></div><div className="two-fields"><NumberField label="Voxel pitch" value={settings.pitch_mm} min={0.000001} onChange={v => setSettings(s => ({ ...s, pitch_mm: v }))} /><NumberField label="Search budget" suffix="sec" value={settings.budget_seconds} min={0.000001} onChange={v => setSettings(s => ({ ...s, budget_seconds: v }))} /></div><p className="hint">Manual pitch changes search resolution. Object dimensions stay physical. The next Start budget includes preparation, fields, search, and validation. Completed import preparation is separate.</p>
          <details><summary>Advanced settings</summary><label className="select-label">CPU threads<select aria-label="CPU threads" value={settings.thread_count ?? 1} onChange={e => setSettings(s => ({ ...s, thread_count: Number(e.target.value) }))}>{Array.from({ length: supportedThreads }, (_, i) => i + 1).map(count => <option key={count} value={count}>{count}</option>)}</select></label><label className="text-label">Host memory ceiling (bytes, optional)<input aria-label="Host memory ceiling" inputMode="numeric" placeholder="Automatic: half of available RAM" value={settings.host_memory_limit_bytes ?? ''} onChange={e => setSettings(s => {
            const next = {...s};
            if (e.target.value === '') delete next.host_memory_limit_bytes;
            else next.host_memory_limit_bytes = e.target.value;
            return next;
          })} /></label><p className="hint">A lower ceiling limits the next Start. Automatic admission uses half of available physical RAM.</p><label className="text-label">Seed (uint64)<input aria-label="Seed" value={settings.seed} onChange={e => setSettings(s => ({ ...s, seed: e.target.value }))} inputMode="numeric" /></label></details>
          {validationError && <p className="field-error" role="alert">{validationError}</p>}
          {pending && <p className="pending-note">Pending settings for the next run. The viewer retains the result's original settings.</p>}

        </section>
      </div><div className="search-actions"><button className="primary" disabled={disabled || !!repair || !accepted || !!validationError} onClick={() => void startPacking()}>▶ Start packing</button><button className="stop" disabled={!native || commandPending || !busy || operation?.kind !== 'solve' || operation.phase === 'stopping'} onClick={() => void run(() => bridge.stop({ operation_id: operation!.id }))}>{operation?.phase === 'stopping' ? 'Stopping…' : 'Stop & keep'}</button></div></aside>
      <div className="center-panel"><Viewer mode={!repair && (live || result) ? 'result' : 'object'} preview={preview} envelopePreview={envelopePreview} placements={displayPoses} dimensions={displayedDimensions} hidden={hidden} selected={selected} onSelect={setSelected} />
        <section className="run-panel" aria-label="Operation status"><div><span className="eyebrow">{busy ? 'NATIVE OPERATION' : 'READY WHEN YOU ARE'}</span><h3>{operation ? `${operation.kind} · ${operation.phase}` : 'Start with an object and a box'}</h3><p>{operation?.detail ?? 'Import an STL, check its dimensions, then start packing.'}</p></div>{resource && <p aria-label="Resource status">Host ceiling {(Number(resource.admitted_host_cap_bytes) / 2 ** 30).toFixed(2)} GiB | Actual threads {resource.actual_threads} | Recoveries {resource.recovery_count}{resource.last_action && <> | {resource.last_action.replaceAll('_', ' ')} ({resource.last_cause})</>}</p>}<div className="elapsed"><strong>{elapsed.toFixed(1)}<small>s</small></strong><span>Total operation elapsed</span></div>{busy && <div className="activity-bar" />}</section>
      </div>
      <aside className="results-panel" aria-label="Packing results"><span className="eyebrow">RESULTS</span><h2>Best found</h2><div className="result-count">{live ? live.count.toLocaleString() : result ? result.document.count.toLocaleString() : '—'}<span>validated copies</span></div>
        <div className="result-status">{live || result ? <><span className="status-dot" /> Native validation: {live?.validation_status ?? result!.document.validation.status}</> : 'No completed result yet'}</div>
        {live && !result && <p className="hint">Complete validated snapshot · revision {live.solution_revision}. {busy ? 'Search continues.' : 'Finalized files unavailable; Save and export are unavailable.'}</p>}
        {result ? <><p className="hint">{busy ? 'Previous completed result while work continues.' : `Complete snapshot · revision ${result.document.solution_revision}`}</p><dl className="result-facts"><div><dt>Utilization</dt><dd>{result.document.metrics.utilization === null ? 'Unavailable' : `${(result.document.metrics.utilization * 100).toFixed(1)}%`}</dd></div><div><dt>Termination</dt><dd>{result.document.metrics.termination_reason.replaceAll('_', ' ')}</dd></div><div><dt>Backend</dt><dd>{result.document.search.resolved_settings.resolved.backend.toUpperCase()}</dd></div><div><dt>CPU threads</dt><dd>{result.document.search.diagnostics?.resource_recovery?.actual_final_threads ?? resultSettings?.thread_count ?? 1}</dd></div><div><dt>Budget scope</dt><dd>{resultSettings?.budget_scope === 'total_start' ? 'From Start' : 'Legacy search only'}</dd></div><div><dt>Pitch</dt><dd>{resultSettings?.pitch_mm} mm</dd></div><div><dt>Search elapsed</dt><dd>{result.document.search.elapsed_seconds.toFixed(2)} s</dd></div></dl>{result.document.search.runtime && <p className="hint">Saved native total before result commit: {result.document.search.runtime.total_elapsed_seconds.toFixed(2)} s. Preparation {result.document.search.runtime.preparation_seconds.toFixed(2)} s · cleanup {result.document.search.runtime.cleanup_seconds.toFixed(2)} s · overrun {result.document.search.runtime.deadline_overrun_seconds.toFixed(2)} s.</p>}<details className="result-settings"><summary>Original result settings</summary><p>Box: {resultSettings?.box_dimensions_mm.join(' × ')} mm</p><p>Clearance: pair {resultSettings?.clearance_mm.pair} / wall {resultSettings?.clearance_mm.wall} mm</p><p>Rotations: {resultSettings?.orientation.mode} · seed {resultSettings?.seed}</p><p>Search budget: {resultSettings?.budget_seconds} s</p><p>Object: {result.document.assets.object.dimensions_mm.join(' × ')} mm</p></details></> : !live && <div className="result-placeholder"><span>▤</span><p>A complete, validated packing will appear here when the engine finishes.</p></div>}
        <div className="inspection"><h3>Inspect copies</h3><label className="select-label">Select copy<select aria-label="Select copy" value={selected ?? ''} onChange={e => setSelected(e.target.value || null)}><option value="">Choose a copy</option>{poses.map(p => <option key={p.copy_id} value={p.copy_id}>{p.copy_id}{hidden.has(p.copy_id) ? ' · hidden' : ''}</option>)}</select></label>{selectedPose && <div className="pose-details"><strong>{selectedPose.copy_id}</strong><p>XYZ: {selectedPose.translation_mm.map(v => v.toFixed(3)).join(', ')} mm</p><p>XYZW: {selectedPose.quaternion_xyzw.map(v => v.toFixed(5)).join(', ')}</p><button onClick={() => setHidden(old => { const next = new Set(old); if (next.has(selectedPose.copy_id)) next.delete(selectedPose.copy_id); else next.add(selectedPose.copy_id); return next; })}>{hidden.has(selectedPose.copy_id) ? 'Show selected' : 'Hide selected'}</button><button onClick={() => setHidden(new Set(poses.filter(p => p.copy_id !== selectedPose.copy_id).map(p => p.copy_id)))}>Isolate</button></div>}{hidden.size > 0 && <button onClick={() => setHidden(new Set())}>Show all ({hidden.size} hidden)</button>}<p className="hint">Hiding and clipping change the view only.</p></div>
        <div className="export-actions"><h3>Export result</h3><button disabled={disabled || !result} onClick={() => void run(() => bridge.export({ format: 'json' }))}>Export result JSON <span>↗</span></button><button disabled={disabled || !result} onClick={() => void run(() => bridge.export({ format: 'stl' }))}>Export packed STL <span>↗</span></button><p className="hint">{report?.packing_envelope ? 'STL export retains the original model and checks it against the validated packing envelope.' : 'Exports use the full accepted geometry and original validated poses.'}</p></div>
      </aside>
    </main><footer className="app-footer"><span><span className="status-dot" />{native ? 'Native desktop connected' : 'Frontend preview · native commands unavailable'}</span><span>Rigid geometry · millimeters · best found</span></footer>
  </div>;
}

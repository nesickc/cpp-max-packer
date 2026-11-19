import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { Desktop } from '../src/generated/contracts';
import fixtures from '../../tests/contracts/schema-fixtures.json';
import { cube45CatalogHash, defaultSettings, snapshotSettings } from '../src/state';

const native = vi.hoisted(() => ({ state: vi.fn(), save: vi.fn(), saveAs: vi.fn(), start: vi.fn(), continue: vi.fn(), stop: vi.fn(), newProject: vi.fn(), importObject: vi.fn(), open: vi.fn(), repair: vi.fn(),
  export: vi.fn(), exportPng: vi.fn(), reveal: vi.fn(), view: null as Uint8Array | null,
  importDropped: vi.fn(), drop: () => {}, onDrop: vi.fn((handler: () => void) => { native.drop = handler; return Promise.resolve(() => {}); }) }));
vi.mock('../src/bridge', () => ({ nativeAvailable: () => true, bridge: native }));
// The viewer's image is whatever `native.view` holds; null means no 3D view.
vi.mock('../src/Viewer', () => ({ Viewer: (props: { dimensions: number[]; preview: { preview_id: string } | null; snapshot?: { current: () => Uint8Array | null } }) => {
  if (props.snapshot) props.snapshot.current = () => native.view;
  return <div data-testid="viewer">{props.dimensions.join(',')} / {props.preview?.preview_id}</div>;
} }));
import { App } from '../src/App';

afterEach(() => { cleanup(); vi.clearAllMocks(); });
describe('UI-01/UI-03 presentation bridge (not native acceptance)', () => {
  it('preserves edits made before the first state response', async () => {
    let respond!: (state: Desktop.State) => void;
    native.state.mockImplementation(() => new Promise<Desktop.State>(resolve => { respond=resolve; }));
    render(<App/>);
    fireEvent.change(screen.getByLabelText('Width'), {target:{value:'420'}});
    respond({desktop_version:1,session_id:'initial-edits',revision:1,object:null,
      draft_settings:structuredClone(defaultSettings),result:null,operation:null,last_error:null});
    await waitFor(()=>expect(screen.getByRole('button',{name:/Import STL/}).hasAttribute('disabled')).toBe(false));
    expect((screen.getByLabelText('Width') as HTMLInputElement).value).toBe('420');
  });
  it.each(['valid','invalid','indeterminate'] as const)('shows repair previews and gates %s acceptance', async status => {
    const fixture=fixtures.find(f=>f.kind==='results'&&f.schema_valid&&f.semantic_valid)!;
    const document=structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
    const report=structuredClone(document.assets.object);
    const artifact={path:'assets/candidate.ply',sha256:'b'.repeat(64)};
    report.repair_proposal={sha256:'a'.repeat(64),path:'assets/recipe.json',algorithm:'manifold',candidate_status:status,
      tolerance_mm:1,max_displacement_mm:null,before:artifact,after:artifact};
    const mesh={format:'ply' as const,sha256:'b'.repeat(64),byte_length:10,triangle_count:12,coordinate_frame:'object_local_mm' as const};
    native.state.mockResolvedValue({desktop_version:1,session_id:'repair-ui',revision:1,
      object:{id:'object',display_name:'Object.stl',report:document.assets.object,preview:{...mesh,preview_id:'original'}},
      draft_settings:structuredClone(defaultSettings),result:null,operation:null,last_error:null,
      repair:{object_id:'object',token:'a'.repeat(64),algorithm:'manifold',tolerance_mm:1,report,
        before:{...mesh,preview_id:'before'},after:{...mesh,preview_id:'after'}}} satisfies Desktop.State);
    native.repair.mockResolvedValue({operation_id:'repair-command'});
    render(<App/>);
    await waitFor(()=>expect(screen.getByTestId('viewer').textContent).toContain('after'));
    fireEvent.change(screen.getByLabelText('Preview geometry'),{target:{value:'before'}});
    expect(screen.getByTestId('viewer').textContent).toContain('before');
    expect(screen.getByRole('button',{name:'▶ Start packing'}).hasAttribute('disabled')).toBe(true);
    const accept=screen.getByRole('button',{name:'Accept repaired geometry'});
    expect(accept.hasAttribute('disabled')).toBe(status!=='valid');
    fireEvent.click(status==='valid' ? accept : screen.getByRole('button',{name:'Reject repair'}));
    await waitFor(()=>expect(native.repair).toHaveBeenCalledWith({action:status==='valid'?'accept':'reject',token:'a'.repeat(64)}));
  });
  it.each(['finished', 'failed', 'canceled'])('preserves unsaved settings through %s import and late completion', async phase => {
    const initial: Desktop.State = { desktop_version:1, session_id:'import-edits', revision:1, object:null,
      draft_settings:structuredClone(defaultSettings), result:null, operation:null, last_error:null };
    native.state.mockResolvedValue(initial);
    let finish!: () => void;
    native.importObject.mockImplementation(() => new Promise<void>(resolve => { finish = resolve; }));
    render(<App/>);
    await waitFor(() => expect(screen.getByRole('button', {name:/Import STL/}).hasAttribute('disabled')).toBe(false));
    fireEvent.change(screen.getByLabelText('Width'), {target:{value:'405'}});
    fireEvent.change(screen.getByLabelText('Between copies'), {target:{value:'2.5'}});
    fireEvent.change(screen.getByLabelText('Voxel pitch'), {target:{value:'3'}});
    fireEvent.change(screen.getByLabelText('Search budget'), {target:{value:'123'}});
    fireEvent.click(screen.getByRole('checkbox', {name:'Include 45° turns'}));
    expect(screen.getByText(/Allowed rotations: 96 orientations/)).toBeDefined();
    fireEvent.change(screen.getByLabelText('Host memory ceiling'), {target:{value:'123456789'}});
    fireEvent.change(screen.getByLabelText('Seed'), {target:{value:'987654321'}});
    fireEvent.click(screen.getByRole('button', {name:/Import STL/}));
    await waitFor(() => expect(native.importObject).toHaveBeenCalledOnce());
    fireEvent.change(screen.getByLabelText('Width'), {target:{value:'410'}});
    // An incomplete field is still an edit and must survive asynchronous events.
    fireEvent.change(screen.getByLabelText('Depth'), {target:{value:''}});
    const next = {...initial, revision:2, draft_settings:{...defaultSettings, pitch_mm:99},
      operation:phase === 'canceled' ? null : {id:'import-1',kind:'import' as const,phase:phase as 'finished'|'failed',
        started_at:'2026-10-18T00:00:00Z',finished_at:'2026-10-18T00:00:01Z',result_id:null,detail:null}};
    native.state.mockResolvedValue(next); finish();
    await waitFor(() => expect(screen.getByRole('button', {name:/Import STL/}).hasAttribute('disabled')).toBe(false));
    for (const [label,value] of Object.entries({'Width':'410','Depth':'','Between copies':'2.5','Voxel pitch':'3',
      'Search budget':'123','Host memory ceiling':'123456789','Seed':'987654321'}))
      expect((screen.getByLabelText(label) as HTMLInputElement).value).toBe(value);
  });
  it('New restores defaults and Open deliberately restores saved settings', async () => {
    const initial: Desktop.State = {desktop_version:1,session_id:'new-open',revision:1,object:null,
      draft_settings:{...defaultSettings,pitch_mm:7},result:null,operation:null,last_error:null};
    native.state.mockResolvedValue(initial);
    native.newProject.mockImplementation(async () => {
      const fresh = {...initial,revision:2,draft_settings:null}; native.state.mockResolvedValue(fresh); return fresh;
    });
    native.open.mockImplementation(async () => {
      native.state.mockResolvedValue({...initial,revision:3,draft_settings:{...defaultSettings,pitch_mm:9},
        operation:{id:'open-1',kind:'open',phase:'finished',started_at:'2026-10-18T00:00:00Z',finished_at:'2026-10-18T00:00:01Z',result_id:null,detail:null}});
    });
    render(<App/>);
    await waitFor(()=>expect((screen.getByLabelText('Voxel pitch') as HTMLInputElement).value).toBe('7'));
    fireEvent.click(screen.getByRole('button',{name:'New'}));
    await waitFor(()=>expect((screen.getByLabelText('Voxel pitch') as HTMLInputElement).value).toBe(String(defaultSettings.pitch_mm)));
    fireEvent.click(screen.getByRole('button',{name:'Open project'}));
    await waitFor(()=>expect((screen.getByLabelText('Voxel pitch') as HTMLInputElement).value).toBe('9'));
  });
  it('T015 displays operational resource recovery and sends an exact lower byte ceiling', async () => {
    const fixture = fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
    const document = structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
    const initial: Desktop.State = {desktop_version:1, session_id:'resource-live', revision:1,
      object:{id:'object', display_name:'Cube.stl', report:document.assets.object, preview:null},
      draft_settings:{...defaultSettings, host_memory_limit_bytes:'134217728'},
      result:{id:'result', document, preview:null, origin:'solve'}, last_error:null,
      operation:{id:'solve-0', kind:'solve', phase:'finished', started_at:'2026-11-14T00:00:00Z',
        finished_at:'2026-11-14T00:01:00Z', result_id:'result', detail:null},
      cpu_runtime:{version:1, supported_min:1, supported_max:8, default_thread_count:8,
        resource_status:{admitted_host_cap_bytes:'5368709120', actual_threads:4, recovery_count:'3',
          last_cause:'VALIDATION_KERNEL_WORK_LIMIT', last_action:'grant_work'}}};
    native.state.mockResolvedValue(initial);
    native.start.mockResolvedValue({operation_id:'next-start'});
    render(<App/>);
    await waitFor(()=>expect(screen.getByLabelText('Search status').textContent).toContain('Host ceiling5.00 GiB'));
    expect(screen.getByLabelText('Search status').textContent).toContain('Recoveries3, last grant work (VALIDATION_KERNEL_WORK_LIMIT)');
    const ceiling = screen.getByLabelText('Host memory ceiling') as HTMLInputElement;
    fireEvent.change(ceiling, {target:{value:'0'}});
    expect(screen.getByRole('button',{name:'▶ Start packing'}).hasAttribute('disabled')).toBe(true);
    fireEvent.change(ceiling, {target:{value:'134217728'}});
    fireEvent.click(screen.getByRole('button',{name:'▶ Start packing'}));
    await waitFor(()=>expect(native.start).toHaveBeenCalledOnce());
    expect(native.start.mock.calls[0][0].settings.host_memory_limit_bytes).toBe('134217728');
  });
  it('SOL-06 Continue adds budget to the displayed result and keeps its box, clearances and rotations', async () => {
    const fixture = fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
    const document = structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
    document.search.resolved_settings.orientation = {mode:'cube'};
    document.count = 1;
    document.placements = [{copy_id:'copy-0', translation_mm:[5,5,5], quaternion_xyzw:[0,0,0,1],
      local_to_world:[[1,0,0,5],[0,1,0,5],[0,0,1,5],[0,0,0,1]]}];
    // A result that was already continued once records both run segments.
    document.search.run_segments.push({...structuredClone(document.search.run_segments[0]), segment_id:'segment-1',
      parent_solution_revision:7, search_state_reset:true});
    const result: Desktop.ResultSnapshot = {id:'result', document, preview:null, origin:'solve'};
    native.state.mockResolvedValue({desktop_version:1, session_id:'continue', revision:1,
      object:{id:'object', display_name:'Cube.stl', report:document.assets.object, preview:null},
      draft_settings:snapshotSettings(result), result, last_error:null,
      operation:{id:'solve-0', kind:'solve', phase:'finished', started_at:'2026-11-18T00:00:00Z',
        finished_at:'2026-11-18T00:01:00Z', result_id:'result', detail:null},
      cpu_runtime:{version:1, supported_min:1, supported_max:8, default_thread_count:4}} satisfies Desktop.State);
    native.continue.mockResolvedValue({operation_id:'continued'});
    render(<App/>);
    const button = () => screen.getByRole('button', {name:'Continue'});
    await waitFor(() => expect(button().hasAttribute('disabled')).toBe(false));
    expect(screen.getByText(/Continued from revision 7/)).toBeDefined();
    // Budget, pitch and seed are free to change.
    fireEvent.change(screen.getByLabelText('Search budget'), {target:{value:'240'}});
    fireEvent.change(screen.getByLabelText('Voxel pitch'), {target:{value:'3'}});
    expect(button().hasAttribute('disabled')).toBe(false);
    // The box is locked: another size leaves only Start, which begins a new job.
    const width = screen.getByLabelText('Width') as HTMLInputElement, original = width.value;
    fireEvent.change(width, {target:{value:String(Number(original) + 1)}});
    expect(button().hasAttribute('disabled')).toBe(true);
    expect(screen.getByText(/Continue keeps the result's box, tolerance, clearances and rotations/)).toBeDefined();
    expect(screen.getByRole('button', {name:'▶ Start packing'}).hasAttribute('disabled')).toBe(false);
    fireEvent.change(width, {target:{value:original}});
    fireEvent.click(screen.getByRole('checkbox', {name:'Include 45° turns'}));
    expect(button().hasAttribute('disabled')).toBe(true);
    fireEvent.click(screen.getByRole('checkbox', {name:'Include 45° turns'}));
    fireEvent.click(button());
    await waitFor(() => expect(native.continue).toHaveBeenCalledOnce());
    expect(native.continue.mock.calls[0][0].settings).toMatchObject({budget_seconds:240, pitch_mm:3, budget_scope:'total_start'});
    expect(native.start).not.toHaveBeenCalled();
  });
  it('SOL-08 starts with the shell default thread count, and Open keeps the recorded one', async () => {
    const fixture = fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
    const document = structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
    const fresh: Desktop.State = {desktop_version:1, session_id:'threads', revision:1, object:null,
      draft_settings:null, result:null, operation:null, last_error:null};
    native.state.mockResolvedValue(fresh);
    render(<App/>);
    // Before an object is prepared the shell has not reported its range yet.
    await waitFor(()=>expect((screen.getByLabelText('CPU threads') as HTMLSelectElement).value).toBe(''));
    expect(screen.getByLabelText('CPU threads').hasAttribute('disabled')).toBe(true);
    const imported: Desktop.State = {...fresh, revision:2,
      object:{id:'object', display_name:'Cube.stl', report:document.assets.object, preview:null},
      cpu_runtime:{version:1, supported_min:1, supported_max:8, default_thread_count:4}};
    native.state.mockResolvedValue(imported);
    await waitFor(()=>expect((screen.getByLabelText('CPU threads') as HTMLSelectElement).value).toBe('4'));
    expect(screen.getByText('CPU · 4 threads')).toBeDefined();
    native.start.mockResolvedValue({operation_id:'default-threads'});
    fireEvent.click(screen.getByRole('button',{name:'▶ Start packing'}));
    await waitFor(()=>expect(native.start).toHaveBeenCalledOnce());
    expect(native.start.mock.calls[0][0].settings.thread_count).toBe(4);
    native.open.mockImplementation(async () => {
      native.state.mockResolvedValue({...imported, revision:3, draft_settings:{...defaultSettings, thread_count:2},
        operation:{id:'open-1',kind:'open',phase:'finished',started_at:'2026-11-14T00:00:00Z',finished_at:'2026-11-14T00:00:01Z',result_id:null,detail:null}});
    });
    fireEvent.click(screen.getByRole('button',{name:'Open project'}));
    await waitFor(()=>expect((screen.getByLabelText('CPU threads') as HTMLSelectElement).value).toBe('2'));
  });
  it('UI-03 search panel shows pitch, threads, RAM, budget and improvement during and after a solve', async () => {
    vi.useFakeTimers({toFake:['Date']});
    try {
      const fixture = fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
      const document = structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
      vi.setSystemTime(new Date('2026-11-14T10:00:30Z'));
      const sent: Desktop.Settings = {...defaultSettings, pitch_mm:3, budget_seconds:120, thread_count:6};
      // The shell echoes the run's settings with sorted keys; equal settings are not pending.
      const echoed = Object.fromEntries(Object.entries(sent).sort(([a],[b]) => a < b ? -1 : 1)) as unknown as Desktop.Settings;
      const live = (count: number, revision: number): Desktop.LiveResult => ({ publication_version:1, session_id:'search-panel',
        native_epoch:'native-1', operation_id:'solve-1', settings_id:'settings-1', solution_revision:revision, sequence:revision, count,
        validation_status:'valid', settings:echoed, preview:null,
        placements:Array.from({length:count},(_,i)=>({...document.placements[0],copy_id:`copy-${i}`})) as Desktop.LiveResult['placements'] });
      const operation: Desktop.Operation = {id:'solve-1', kind:'solve', phase:'improving', started_at:'2026-11-14T10:00:00Z',
        finished_at:null, result_id:null, detail:null};
      const status = (estimated: string): NonNullable<Desktop.State['cpu_runtime']> => ({version:1, supported_min:1, supported_max:8,
        default_thread_count:4, resource_status:{admitted_host_cap_bytes:'8589934592', estimated_working_bytes:estimated,
          actual_threads:6, recovery_count:'0', last_cause:'', last_action:''}});
      // An empty live layout is not an improvement.
      const running: Desktop.State = {desktop_version:1, session_id:'search-panel', revision:1,
        object:{id:'object', display_name:'Cube.stl', report:document.assets.object, preview:null},
        draft_settings:sent, result:null, live_result:live(0, 1), operation, last_error:null, cpu_runtime:status('966367641')};
      native.state.mockResolvedValue(running);
      render(<App/>);
      const panel = () => screen.getByLabelText('Search status').textContent;
      await waitFor(()=>expect(panel()).toContain('Remaining budget90.0 s'));
      for (const fact of ['Pitch3 mm', 'Threads6', 'Estimated RAM921.6 MiB', 'Host ceiling8.00 GiB', 'Since improvementNo packing yet'])
        expect(panel()).toContain(fact);
      native.state.mockResolvedValue({...running, revision:2, live_result:live(24, 2)});
      await waitFor(()=>expect(panel()).toContain('Since improvement0.0 s'));
      expect(screen.queryByText(/Pending settings for the next run/)).toBeNull();
      vi.setSystemTime(new Date('2026-11-14T10:00:35Z'));
      await waitFor(()=>expect(panel()).toContain('Since improvement5.0 s'));
      expect(panel()).toContain('Remaining budget85.0 s');
      native.state.mockResolvedValue({...running, revision:3, live_result:live(25, 3)});
      await waitFor(()=>expect(panel()).toContain('Since improvement0.0 s'));

      document.search.resolved_settings.resolved = {...document.search.resolved_settings.resolved, pitch_mm:3, thread_count:6};
      document.search.resolved_settings.resolution = {mode:'manual', pitch_mm:3};
      document.search.resolved_settings.search.budget_seconds = 120;
      document.search.resolved_settings.search.budget_scope = 'total_start';
      document.search.pitch_levels_mm = [3, 6];
      document.search.elapsed_seconds = 50;
      document.search.runtime = {total_elapsed_seconds:52, preparation_seconds:1, cleanup_seconds:0, deadline_overrun_seconds:0} as unknown as typeof document.search.runtime;
      document.metrics.time_to_best_seconds = 38;
      const finished: Desktop.State = {...running, revision:4, live_result:live(25, 3), result:{id:'result-1', document, preview:null, origin:'solve'},
        operation:{...operation, phase:'finished', finished_at:'2026-11-14T10:00:52Z', result_id:'result-1', completion_elapsed_seconds:52}};
      native.state.mockResolvedValue(finished);
      await waitFor(()=>expect(panel()).toContain('Since improvement12.0 s'));
      for (const fact of ['Pitch3 mm, coarsened to 6 mm', 'Threads6', 'Estimated RAM921.6 MiB', 'Remaining budget68.0 s'])
        expect(panel()).toContain(fact);
      // The result still shows all its own settings.
      expect(screen.getByText('Pitch: 3 mm · CPU threads: 6')).toBeDefined();
      // A search-only budget excludes preparation.
      const searchOnly = structuredClone(document);
      searchOnly.search.resolved_settings.search.budget_scope = 'search_only';
      native.state.mockResolvedValue({...finished, revision:5, result:{id:'result-1', document:searchOnly, preview:null, origin:'solve'}});
      await waitFor(()=>expect(panel()).toContain('Remaining budget70.0 s'));
      // A later solve that kept this result does not lend it its memory status.
      native.state.mockResolvedValue({...finished, revision:6, cpu_runtime:status('12345678'),
        operation:{...operation, id:'solve-2', phase:'finished', finished_at:'2026-11-14T10:02:00Z', completion_elapsed_seconds:3}});
      await waitFor(()=>expect(panel()).toContain('Estimated RAM—'));
      expect(panel()).toContain('Host ceiling—');
      expect(panel()).toContain('Pitch3 mm, coarsened to 6 mm');
    } finally { vi.useRealTimers(); }
  });
  it('UI-02 T015 shows the native live packing while Save and export stay unavailable', async () => {
    const fixture = fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
    const document = structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
    const live: Desktop.LiveResult = { publication_version:1, session_id:'session-live', native_epoch:'native-1',
      operation_id:'solve-live', settings_id:'settings-1', solution_revision:1, sequence:1, count:24,
      validation_status:'valid', settings:structuredClone(defaultSettings), preview:null,
      placements:Array.from({length:24},(_,i)=>({...document.placements[0],copy_id:`copy-${i}`})) as Desktop.LiveResult['placements'] };
    native.state.mockResolvedValue({ desktop_version:1,session_id:'session-live',revision:1,
      object:{id:'object',display_name:'Cube.stl',report:document.assets.object,preview:null},
      draft_settings:defaultSettings,result:null,live_result:live,last_error:null,
      operation:{id:'solve-live',kind:'solve',phase:'placing',started_at:'2026-10-11T00:00:00Z',
        finished_at:null,result_id:null,detail:null} } satisfies Desktop.State);
    render(<App/>);
    await waitFor(()=>expect(screen.getByText('24')).toBeDefined());
    expect(screen.getByRole('option',{name:'copy-23'})).toBeDefined();
    for (const name of ['Save project', 'Save as…', /Export result JSON/, /Export view PNG/])
      expect(screen.getByRole('button',{name}).hasAttribute('disabled')).toBe(true);
  });
  it('uses 45-degree rotations for the next Start while retaining a legacy fixed result', async () => {
    const fixture=fixtures.find(f=>f.kind==='results'&&f.schema_valid&&f.semantic_valid)!;
    const document=structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
    delete document.search.resolved_settings.search.budget_scope;
    const previous:Desktop.ResultSnapshot={id:'legacy-result',document,preview:null,origin:'project'};
    const initial:Desktop.State={desktop_version:1,session_id:'legacy-start',revision:1,
      object:{id:'legacy-object',display_name:'Object.stl',report:document.assets.object,preview:null},
      draft_settings:snapshotSettings(previous),result:previous,operation:null,last_error:null};
    native.state.mockResolvedValue(initial);
    const next=structuredClone(initial);
    next.revision=2;
    next.result!.id='new-result';
    next.result!.document.search.resolved_settings.search.budget_scope='total_start';
    next.result!.document.search.resolved_settings.orientation={mode:'custom',quaternions_xyzw:[[0,0,0,1]]};
    next.result!.document.search.resolved_settings.resolved.orientation_catalog_sha256=cube45CatalogHash;
    native.start.mockImplementation(async()=>{native.state.mockResolvedValue(next);return {operation_id:'new-start'};});
    render(<App/>);
    await waitFor(()=>expect(screen.getByRole('button',{name:'▶ Start packing'}).hasAttribute('disabled')).toBe(false));
    expect(screen.getByText(/Pending settings for the next run/)).toBeDefined();
    expect((screen.getByRole('checkbox', {name:'Include 45° turns'}) as HTMLInputElement).checked).toBe(false);
    fireEvent.click(screen.getByRole('checkbox', {name:'Include 45° turns'}));
    fireEvent.click(screen.getByRole('button',{name:'▶ Start packing'}));
    await waitFor(()=>expect(native.start).toHaveBeenCalledOnce());
    await waitFor(()=>expect(screen.getByText('From Start')).toBeDefined());
    await waitFor(()=>expect(screen.queryByText(/Pending settings for the next run/)).toBeNull());
    expect(native.start.mock.calls[0][0].settings.budget_scope).toBe('total_start');
    expect(native.start.mock.calls[0][0].settings.orientation).toEqual({mode:'cube45'});
    expect(document.search.resolved_settings.orientation.mode).toBe('fixed');
    expect(document.search.resolved_settings.search.budget_scope).toBeUndefined();
  });
  it('shows the retained native cause and a pitch suggestion without changing settings', async () => {
    const initial: Desktop.State = { desktop_version: 1, session_id: 'session-failure', revision: 1, object: null, draft_settings: structuredClone(defaultSettings), result: null, operation: null, last_error: { code: 'RESOURCE_LIMIT', message: 'Search stopped; the valid result was retained.', recoverable: true, details: { failure: { phase: 'spectral_preflight', cause_code: 'GRID_CELL_LIMIT', resource: { name: 'grid_cells', required: '40611648', limit: '16777216' }, suggested_pitch_mm: 5 } } } };
    native.state.mockResolvedValue(initial);
    render(<App />);
    await waitFor(() => expect(screen.getByRole('alert').textContent).toContain('GRID_CELL_LIMIT'));
    expect(screen.getByRole('alert').textContent).toContain('40611648');
    expect(screen.getByRole('alert').textContent).toContain('Try a voxel pitch of 5 mm');
    expect((screen.getByLabelText('Voxel pitch') as HTMLInputElement).value).toBe(String(defaultSettings.pitch_mm));
    expect(screen.getByText('Diagnostic details')).toBeDefined();
  });
  it('saves edited pending settings while old result viewer keeps original box/preview', async () => {
    const fixture = fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
    const document = structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
    document.container = { kind: 'box', dimensions_mm: [10,20,30] };
    document.search.resolved_settings.container = { kind: 'box', dimensions_mm: [10,20,30] };
    const preview = { preview_id: 'result-mesh', format: 'ply' as const, sha256: 'a'.repeat(64), byte_length: 10, triangle_count: 12, coordinate_frame: 'object_local_mm' as const };
    const initial: Desktop.State = { desktop_version: 1, session_id: 'session-ui', revision: 1, object: { id: 'object-current', display_name: 'Object.stl', report: document.assets.object, preview: { ...preview, preview_id: 'new-import-mesh' } }, draft_settings: structuredClone(defaultSettings), result: { id: 'result-previous', document, preview, origin: 'solve' }, operation: null, last_error: null };
    native.state.mockResolvedValue(initial); native.save.mockResolvedValue({ operation_id: 'save-test' });
    render(<App />);
    await waitFor(() => expect(screen.getByRole('button', { name: 'Save project' }).hasAttribute('disabled')).toBe(false));
    fireEvent.change(screen.getByLabelText('Width'), { target: { value: '125' } });
    fireEvent.change(screen.getByLabelText('Between copies'), { target: { value: '2.5' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save project' }));
    await waitFor(() => expect(native.save).toHaveBeenCalledOnce());
    expect(native.save.mock.calls[0][0].settings.box_dimensions_mm).toEqual([125,100,100]);
    expect(native.save.mock.calls[0][0].settings.clearance_mm).toEqual({ pair: 2.5, wall: 1 });
    expect(screen.getByTestId('viewer').textContent).toBe('10,20,30 / result-mesh');
    expect(document.container.dimensions_mm).toEqual([10,20,30]);
  });
  it('shows a tolerant result in its validated interior', async () => {
    const fixture = fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
    const document = structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
    document.container = { kind: 'box', dimensions_mm: [10.5,20.5,30.5] };
    document.search.resolved_settings.container = { kind: 'box', dimensions_mm: [10,20,30], tolerance_mm: .5 };
    const preview = { preview_id: 'result-mesh', format: 'ply' as const, sha256: 'a'.repeat(64), byte_length: 10, triangle_count: 12, coordinate_frame: 'object_local_mm' as const };
    const initial: Desktop.State = { desktop_version: 1, session_id: 'session-ui', revision: 1, object: { id: 'object-current', display_name: 'Object.stl', report: document.assets.object, preview }, draft_settings: structuredClone(defaultSettings), result: { id: 'result-previous', document, preview, origin: 'solve' }, operation: null, last_error: null };
    native.state.mockResolvedValue(initial);
    render(<App />);
    await waitFor(() => expect(screen.getByTestId('viewer').textContent).toBe('10.5,20.5,30.5 / result-mesh'));
  });
});

it('creates a packing envelope without user-supplied geometry parameters',async()=>{
  const fixture=fixtures.find(f=>f.kind==='results'&&f.schema_valid&&f.semantic_valid)!;
  const document=structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
  native.state.mockResolvedValue({desktop_version:1,session_id:'envelope-ui',revision:1,
    object:{id:'object',display_name:'Model.stl',report:document.assets.object,preview:null},
    draft_settings:structuredClone(defaultSettings),result:null,operation:null,last_error:null} satisfies Desktop.State);
  native.repair.mockResolvedValue({operation_id:'envelope'});
  render(<App/>);
  const button=await screen.findByRole('button',{name:'Create packing envelope',hidden:true});
  fireEvent.click(button);
  await waitFor(()=>expect(native.repair).toHaveBeenCalledWith({action:'envelope'}));
});

it('imports a dropped STL with the chosen units, and refuses a drop while an operation runs', async () => {
  const idle: Desktop.State = {desktop_version:1,session_id:'drop',revision:1,object:null,
    draft_settings:structuredClone(defaultSettings),result:null,operation:null,last_error:null};
  native.state.mockResolvedValue(idle);
  native.importDropped.mockResolvedValue({operation_id:'import-1'});
  render(<App/>);
  await waitFor(() => expect(screen.getByRole('button', {name:/Import STL/}).hasAttribute('disabled')).toBe(false));
  fireEvent.change(screen.getByLabelText('Source units'), {target:{value:'inch'}});
  act(() => native.drop());
  await waitFor(() => expect(native.importDropped).toHaveBeenCalledWith({units:'inch'}));
  expect(native.importObject).not.toHaveBeenCalled();

  native.importDropped.mockRejectedValue({code:'DROP_REFUSED',message:'Only an STL file can be dropped.'});
  act(() => native.drop());
  expect((await screen.findByRole('alert')).textContent).toContain('Only an STL file can be dropped.');

  native.importDropped.mockClear();
  native.state.mockResolvedValue({...idle, revision:2, operation:{id:'solve-1',kind:'solve',phase:'running',
    started_at:'2026-11-12T00:00:00Z',finished_at:null,result_id:null,detail:null}});
  await waitFor(() => expect(screen.getByRole('button', {name:/Import STL/}).hasAttribute('disabled')).toBe(true));
  act(() => native.drop());
  expect((await screen.findByRole('alert')).textContent).toContain('Wait for the current operation to finish');
  expect(native.importDropped).not.toHaveBeenCalled();
});

it('UI-03 shows a long export\'s progress and cancels it without an error', async () => {
  const fixture = fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
  const document = structuredClone(fixture.value) as unknown as Desktop.ResultSnapshot['document'];
  const exporting: Desktop.Operation = {id:'export-1', kind:'export', phase:'validating', started_at:'2026-11-17T10:00:00Z',
    finished_at:null, result_id:'result', detail:null, progress:{stage:'Checking the exported copies', done:212, total:429}};
  const importing: Desktop.State = {desktop_version:1, session_id:'long-export', revision:1,
    object:{id:'object', display_name:'Cube.stl', report:document.assets.object, preview:null},
    draft_settings:structuredClone(defaultSettings), result:{id:'result', document, preview:null, origin:'solve'},
    operation:{...exporting, kind:'import', phase:'preparing', progress:{stage:'Wrapping the packing envelope'}}, last_error:null};
  native.state.mockResolvedValue(importing);
  render(<App/>);
  // A stage with no known size keeps the indeterminate bar.
  await waitFor(() => expect(screen.getByText('Wrapping the packing envelope')).toBeDefined());
  expect(screen.queryByRole('progressbar')).toBeNull();
  native.state.mockResolvedValue({...importing, revision:2, operation:exporting});
  await waitFor(() => expect(screen.getByText('Checking the exported copies · 49%')).toBeDefined());
  const bar = screen.getByRole('progressbar', {name:'Operation progress'});
  expect([bar.getAttribute('aria-valuenow'), bar.getAttribute('aria-valuemax')]).toEqual(['212', '429']);
  native.stop.mockResolvedValue({operation_id:'export-1', accepted:true});
  fireEvent.click(screen.getByRole('button', {name:'Cancel'}));
  await waitFor(() => expect(native.stop).toHaveBeenCalledWith({operation_id:'export-1'}));
  native.state.mockResolvedValue({...importing, revision:3, operation:{...exporting, phase:'stopping'}});
  await waitFor(() => expect(screen.getByRole('button', {name:'Cancelling…'}).hasAttribute('disabled')).toBe(true));
  const { progress: _, ...ended } = exporting;
  native.state.mockResolvedValue({...importing, revision:4, operation:{...ended, phase:'cancelled', finished_at:'2026-11-17T10:00:05Z',
    detail:'Cancelled; nothing was changed.'}});
  await waitFor(() => expect(screen.getByText('export · cancelled')).toBeDefined());
  expect(screen.getByText('Cancelled; nothing was changed.')).toBeDefined();
  expect(screen.queryByRole('alert')).toBeNull();
  expect(screen.getByRole('button', {name:/Export packed STL/}).hasAttribute('disabled')).toBe(false);
  expect(screen.getByRole('button', {name:'Stop & keep'}).hasAttribute('disabled')).toBe(true);
});

describe('§7.1 project and export actions', () => {
  const fixture = () => fixtures.find(f => f.kind === 'results' && f.schema_valid && f.semantic_valid)!;
  const finished = (kind: Desktop.Operation['kind'], phase: Desktop.Operation['phase'] = 'finished', detail: string | null = null): Desktop.Operation =>
    ({id:`${kind}-1`, kind, phase, started_at:'2026-11-19T10:00:00Z', finished_at:'2026-11-19T10:00:01Z', result_id:'result', detail});
  const solved = (): Desktop.State => {
    const document = structuredClone(fixture().value) as unknown as Desktop.ResultSnapshot['document'];
    return {desktop_version:1, session_id:'actions', revision:1,
      object:{id:'object', display_name:'Cube.stl', report:document.assets.object, preview:null},
      draft_settings:structuredClone(defaultSettings), result:{id:'result', document, preview:null, origin:'solve'},
      operation:finished('solve'), last_error:null};
  };
  it('Save writes the current project and Save as always asks, with the same availability', async () => {
    native.state.mockResolvedValue(solved());
    native.save.mockResolvedValue({operation_id:'save-1'}); native.saveAs.mockResolvedValue(null);
    render(<App/>);
    await waitFor(() => expect(screen.getByRole('button', {name:'Save as…'}).hasAttribute('disabled')).toBe(false));
    fireEvent.change(screen.getByLabelText('Search budget'), {target:{value:'90'}});
    fireEvent.click(screen.getByRole('button', {name:'Save project'}));
    await waitFor(() => expect(native.save).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('button', {name:'Save as…'}));
    await waitFor(() => expect(native.saveAs).toHaveBeenCalledOnce());
    expect(native.saveAs.mock.calls[0][0].settings.budget_seconds).toBe(90);
    expect(native.save.mock.calls[0][0]).toEqual(native.saveAs.mock.calls[0][0]);
    // Invalid settings block both.
    fireEvent.change(screen.getByLabelText('Search budget'), {target:{value:'0'}});
    for (const name of ['Save project', 'Save as…']) expect(screen.getByRole('button', {name}).hasAttribute('disabled')).toBe(true);
  });
  it('Export view PNG sends the viewer image of the displayed result', async () => {
    native.state.mockResolvedValue(solved());
    native.view = Uint8Array.of(0x89, 0x50, 0x4e, 0x47);
    native.exportPng.mockResolvedValue({operation_id:'export-png'});
    render(<App/>);
    const button = () => screen.getByRole('button', {name:/Export view PNG/});
    await waitFor(() => expect(button().hasAttribute('disabled')).toBe(false));
    fireEvent.click(button());
    await waitFor(() => expect(native.exportPng).toHaveBeenCalledWith('result', native.view));
    // Without a 3D view there is no image: a message, and nothing is sent.
    native.view = null; native.exportPng.mockClear();
    fireEvent.click(button());
    expect((await screen.findByRole('alert')).textContent).toContain('The 3D view is unavailable');
    expect(native.exportPng).not.toHaveBeenCalled();
  });
  it('a finished save or export can show its folder until the next operation', async () => {
    const state = solved();
    native.state.mockResolvedValue(state); native.reveal.mockResolvedValue(undefined);
    render(<App/>);
    await waitFor(() => expect(screen.getByRole('button', {name:/Export view PNG/}).hasAttribute('disabled')).toBe(false));
    expect(screen.queryByRole('button', {name:'Show in folder'})).toBeNull();
    native.state.mockResolvedValue({...state, revision:2, operation:finished('export', 'finished', 'Output: D:\\out\\view.png')});
    fireEvent.click(await screen.findByRole('button', {name:'Show in folder'}));
    await waitFor(() => expect(native.reveal).toHaveBeenCalledOnce());
    expect(screen.getByText('Output: D:\\out\\view.png')).toBeDefined();
    native.state.mockResolvedValue({...state, revision:3, operation:finished('export', 'failed', 'Export failed.')});
    await waitFor(() => expect(screen.queryByRole('button', {name:'Show in folder'})).toBeNull());
    native.state.mockResolvedValue({...state, revision:4, operation:finished('save', 'finished', 'Saved to D:\\p.spectrapack')});
    expect(await screen.findByRole('button', {name:'Show in folder'})).toBeDefined();
    native.state.mockResolvedValue({...state, revision:5, operation:{...finished('solve'), phase:'placing', finished_at:null}});
    await waitFor(() => expect(screen.queryByRole('button', {name:'Show in folder'})).toBeNull());
  });
});

it('fills the box from a preset and keeps the dimensions editable', async () => {
  native.state.mockResolvedValue({desktop_version:1,session_id:'presets',revision:1,object:null,
    draft_settings:structuredClone(defaultSettings),result:null,operation:null,last_error:null} satisfies Desktop.State);
  render(<App/>);
  const preset = await screen.findByLabelText('Box preset') as HTMLSelectElement;
  expect(preset.value).toBe('');
  fireEvent.change(preset, {target:{value:'Nova Post 10 kg'}});
  expect(['Width','Depth','Height'].map(label => (screen.getByLabelText(label) as HTMLInputElement).value)).toEqual(['400','340','285']);
  expect(preset.value).toBe('Nova Post 10 kg');
  fireEvent.change(screen.getByLabelText('Depth'), {target:{value:'338'}});
  expect(preset.value).toBe('');
  expect((screen.getByLabelText('Width') as HTMLInputElement).value).toBe('400');
  fireEvent.change(screen.getByLabelText('Depth'), {target:{value:'340'}});
  expect(preset.value).toBe('Nova Post 10 kg');
});

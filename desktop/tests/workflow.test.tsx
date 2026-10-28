import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { Desktop } from '../src/generated/contracts';
import fixtures from '../../tests/contracts/schema-fixtures.json';
import { cube45CatalogHash, defaultSettings, snapshotSettings } from '../src/state';

const native = vi.hoisted(() => ({ state: vi.fn(), save: vi.fn(), start: vi.fn(), newProject: vi.fn(), importObject: vi.fn(), open: vi.fn(), repair: vi.fn() }));
vi.mock('../src/bridge', () => ({ nativeAvailable: () => true, bridge: native }));
vi.mock('../src/Viewer', () => ({ Viewer: (props: { dimensions: number[]; preview: { preview_id: string } | null }) => <div data-testid="viewer">{props.dimensions.join(',')} / {props.preview?.preview_id}</div> }));
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
      draft_settings:{...defaultSettings, host_memory_limit_bytes:'134217728'}, result:null, operation:null, last_error:null,
      cpu_runtime:{version:1, supported_min:1, supported_max:8, default_thread_count:8,
        resource_status:{admitted_host_cap_bytes:'5368709120', actual_threads:4, recovery_count:'3',
          last_cause:'VALIDATION_KERNEL_WORK_LIMIT', last_action:'grant_work'}}};
    native.state.mockResolvedValue(initial);
    native.start.mockResolvedValue({operation_id:'next-start'});
    render(<App/>);
    await waitFor(()=>expect(screen.getByLabelText('Resource status').textContent).toContain('Actual threads 4'));
    expect(screen.getByLabelText('Resource status').textContent).toContain('5.00 GiB');
    expect(screen.getByLabelText('Resource status').textContent).toContain('Recoveries 3');
    const ceiling = screen.getByLabelText('Host memory ceiling') as HTMLInputElement;
    fireEvent.change(ceiling, {target:{value:'0'}});
    expect(screen.getByRole('button',{name:'▶ Start packing'}).hasAttribute('disabled')).toBe(true);
    fireEvent.change(ceiling, {target:{value:'134217728'}});
    fireEvent.click(screen.getByRole('button',{name:'▶ Start packing'}));
    await waitFor(()=>expect(native.start).toHaveBeenCalledOnce());
    expect(native.start.mock.calls[0][0].settings.host_memory_limit_bytes).toBe('134217728');
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
    expect(screen.getByRole('button',{name:'Save project'}).hasAttribute('disabled')).toBe(true);
    expect(screen.getByRole('button',{name:/Export result JSON/}).hasAttribute('disabled')).toBe(true);
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

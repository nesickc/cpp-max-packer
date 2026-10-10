import { act, cleanup, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';

const renderState = vi.hoisted(() => ({ tick: null as FrameRequestCallback | null, render: vi.fn() }));
vi.mock('three', async importOriginal => {
  const actual = await importOriginal<typeof import('three')>();
  return { ...actual, WebGLRenderer: class {
    domElement = document.createElement('canvas');
    setPixelRatio() {}
    setSize() {}
    render = renderState.render;
    dispose() {}
  } };
});
import { Viewer } from '../src/Viewer';

afterEach(() => { cleanup(); vi.unstubAllGlobals(); vi.clearAllMocks(); });
describe('UI-02/UI-03 renderer failure presentation', () => {
  it('stops a failing render loop and exposes the error with native actions available', () => {
    vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} });
    vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => { renderState.tick = callback; return 1; });
    vi.stubGlobal('cancelAnimationFrame', () => { renderState.tick = null; });
    renderState.render.mockImplementation(() => { throw new Error('Injected GPU upload failure'); });
    render(<Viewer mode="result" preview={null} placements={[]} dimensions={[40,40,40]} hidden={new Set()} selected={null} onSelect={() => {}} />);
    expect(renderState.tick).not.toBeNull();
    act(() => { const tick = renderState.tick!; renderState.tick = null; tick(0); });
    expect(screen.getByRole('alert').textContent).toContain('Injected GPU upload failure');
    expect(screen.getByRole('alert').textContent).toContain('Packing, project actions and export remain available');
    expect(renderState.tick).toBeNull();
  });
});

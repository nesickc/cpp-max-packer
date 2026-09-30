import { useEffect, useRef, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { PLYLoader } from 'three/addons/loaders/PLYLoader.js';
import type { Desktop, Results } from './generated/contracts';
import { bridge } from './bridge';
import { packingMesh } from './transforms';

interface Props {
  mode: 'object' | 'result';
  preview: Desktop.ImportedObject['preview'];
  placements: Results.Placement[];
  dimensions: [number, number, number];
  hidden: ReadonlySet<string>;
  selected: string | null;
  onSelect: (id: string | null) => void;
}

export function Viewer({ mode, preview, placements, dimensions, hidden, selected, onSelect }: Props) {
  const host = useRef<HTMLDivElement>(null);
  const fitAction = useRef<() => void>(() => {});
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [clip, setClip] = useState(100);
  const [opacity, setOpacity] = useState(12);
  const [orthographic, setOrthographic] = useState(false);
  const clipRef = useRef(clip); clipRef.current = clip;
  const opacityRef = useRef(opacity); opacityRef.current = opacity;
  const selectedRef = useRef(selected); selectedRef.current = selected;

  useEffect(() => {
    const element = host.current!;
    let renderer: THREE.WebGLRenderer;
    setError(null);
    try { renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true }); }
    catch { setError('3D acceleration is unavailable. Packing, project actions and export remain available.'); return; }
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.localClippingEnabled = true;
    element.appendChild(renderer.domElement);
    const scene = new THREE.Scene();
    const [w, d, h] = dimensions;
    const size = Math.max(w, d, h, 1);
    const camera = orthographic ? new THREE.OrthographicCamera(-size, size, size, -size, .01, size * 100) : new THREE.PerspectiveCamera(42, 1, .01, size * 100);
    camera.up.set(0, 0, 1);
    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.maxDistance = size * 20;
    controls.minDistance = size * .01;
    const fit = () => { controls.target.set(w / 2, d / 2, h / 2); camera.position.set(w / 2 + size * 1.7, d / 2 - size * 2.1, h / 2 + size * 1.5); camera.lookAt(controls.target); camera.updateProjectionMatrix(); controls.update(); };
    fitAction.current = fit; fit();
    scene.add(new THREE.HemisphereLight('#eefaff', '#35464e', 2.7));
    const light = new THREE.DirectionalLight('#ffffff', 3); light.position.set(size, -size, size * 3); scene.add(light);
    const boxGeometry = new THREE.BoxGeometry(w, d, h);
    const boxMaterial = new THREE.MeshBasicMaterial({ color: '#7d9cb5', transparent: true, opacity: opacityRef.current / 100, side: THREE.DoubleSide, depthWrite: false });
    const box = new THREE.Mesh(boxGeometry, boxMaterial); box.position.set(w / 2, d / 2, h / 2); box.renderOrder = 2; scene.add(box);
    const edgesGeometry = new THREE.EdgesGeometry(boxGeometry);
    const edgesMaterial = new THREE.LineBasicMaterial({ color: '#8cacc5', transparent: true, opacity: .52 });
    const edges = new THREE.LineSegments(edgesGeometry, edgesMaterial); edges.position.copy(box.position); scene.add(edges);
    const grid = new THREE.GridHelper(size * 2, 20, '#3b6472', '#213443'); grid.rotation.x = Math.PI / 2; grid.position.set(w / 2, d / 2, -.02); scene.add(grid);
    const axes = new THREE.AxesHelper(size * .2); scene.add(axes);
    const plane = new THREE.Plane(new THREE.Vector3(0, 0, -1), h);
    const raycaster = new THREE.Raycaster();
    let mesh: THREE.InstancedMesh | null = null;
    let geometry: THREE.BufferGeometry | null = null;
    let ids: string[] = [];
    let disposed = false;
    const install = (g: THREE.BufferGeometry) => {
      geometry = g;
      const packing = packingMesh(g, placements, hidden); mesh = packing.mesh; ids = packing.ids;
      const material = mesh.material as THREE.MeshStandardMaterial; material.clippingPlanes = [plane];
      ids.forEach((id, i) => mesh!.setColorAt(i, new THREE.Color(id === selected ? '#ffbb6b' : '#52caba')));
      if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
      scene.add(mesh);
    };
    setLoading(!!preview);
    if (preview) bridge.preview({ preview_id: preview.preview_id }).then(bytes => {
      if (disposed) return;
      const data = bytes instanceof ArrayBuffer ? bytes : new Uint8Array(bytes).buffer;
      if (data.byteLength !== preview.byte_length) throw new Error('Preview length differs from its registered descriptor.');
      const parsed = new PLYLoader().parse(data);
      if (!parsed.index || parsed.index.count / 3 !== preview.triangle_count) { parsed.dispose(); throw new Error('Preview triangle/index metadata differs from its registered descriptor.'); }
      parsed.computeVertexNormals(); install(parsed);
    }).catch(e => { if (!disposed) setError(`Preview unavailable: ${String(e)}. Native export remains available.`); }).finally(() => { if (!disposed) setLoading(false); });
    const resize = () => {
      const width = Math.max(element.clientWidth, 1), height = Math.max(element.clientHeight, 1);
      renderer.setSize(width, height);
      if (camera instanceof THREE.PerspectiveCamera) camera.aspect = width / height;
      else { camera.left = -size * width / height; camera.right = size * width / height; }
      camera.updateProjectionMatrix();
    };
    const observer = new ResizeObserver(resize); observer.observe(element); resize();
    let down: [number, number] | null = null;
    const pointerDown = (event: PointerEvent) => { down = [event.clientX, event.clientY]; };
    const pointerUp = (event: PointerEvent) => {
      if (!down || Math.hypot(event.clientX - down[0], event.clientY - down[1]) > 5 || !mesh) return;
      const rect = renderer.domElement.getBoundingClientRect();
      raycaster.setFromCamera(new THREE.Vector2((event.clientX - rect.left) / rect.width * 2 - 1, -(event.clientY - rect.top) / rect.height * 2 + 1), camera);
      const hit = raycaster.intersectObject(mesh).find(hit => hit.instanceId !== undefined && hit.point.z <= h * clipRef.current / 100);
      onSelect(hit?.instanceId === undefined ? null : ids[hit.instanceId]);
    };
    renderer.domElement.addEventListener('pointerdown', pointerDown); renderer.domElement.addEventListener('pointerup', pointerUp);
    let coloredSelection = selected;
    renderer.setAnimationLoop(() => {
      plane.constant = h * clipRef.current / 100; boxMaterial.opacity = opacityRef.current / 100;
      if (mesh && coloredSelection !== selectedRef.current) {
        coloredSelection = selectedRef.current;
        ids.forEach((id, i) => mesh!.setColorAt(i, new THREE.Color(id === coloredSelection ? '#ffbb6b' : '#52caba')));
        if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
      }
      try { controls.update(); renderer.render(scene, camera); }
      catch (e) {
        renderer.setAnimationLoop(null);
        if (!disposed) setError(`3D rendering unavailable: ${String(e)}. Packing, project actions and export remain available.`);
      }
    });
    return () => {
      disposed = true; renderer.setAnimationLoop(null); observer.disconnect(); controls.dispose();
      renderer.domElement.removeEventListener('pointerdown', pointerDown); renderer.domElement.removeEventListener('pointerup', pointerUp);
      mesh?.dispose(); (mesh?.material as THREE.Material | undefined)?.dispose(); geometry?.dispose();
      boxGeometry.dispose(); boxMaterial.dispose(); edgesGeometry.dispose(); edgesMaterial.dispose(); grid.geometry.dispose();
      const gridMaterials = Array.isArray(grid.material) ? grid.material : [grid.material]; gridMaterials.forEach(m => m.dispose());
      axes.geometry.dispose(); (Array.isArray(axes.material) ? axes.material : [axes.material]).forEach(m => m.dispose());
      renderer.dispose(); renderer.domElement.remove();
    };
  }, [preview?.preview_id, placements, dimensions, hidden, orthographic, onSelect]);

  return <section className="viewer" aria-label="Packing viewer">
    <div className="viewer-heading"><div><span className="eyebrow">WORKSPACE</span><h2>{mode === 'result' ? 'Validated packing' : preview ? 'Object preview' : 'Packing viewer'}</h2></div><span className="frame-tag">mm · Z up</span></div>
    <div className="viewport" ref={host} aria-label="3D packing canvas" />
    {!preview && !error && <div className="empty-view"><span className="empty-symbol">◇</span><h3>Import an STL to begin</h3><p>The box shows the usable interior volume.<br />Completed results retain their original settings.</p></div>}
    {(loading || error) && <div className="viewer-notice" role={error ? 'alert' : 'status'}>{error ?? 'Loading shared display mesh…'}</div>}
    <div className="dimension-label">{dimensions.map(v => Number.isFinite(v) ? v : '—').join(' × ')} mm <span>internal volume</span></div>
    <div className="viewer-toolbar"><button onClick={() => fitAction.current()} title="Fit to container">↗ Fit view</button><button onClick={() => setOrthographic(v => !v)}>{orthographic ? 'Orthographic' : 'Perspective'}</button><label>Box <input aria-label="Container transparency" type="range" min="0" max="40" value={opacity} onChange={e => setOpacity(Number(e.target.value))} /></label><label>Clip Z <input aria-label="Clipping height" type="range" min="0" max="100" value={clip} onChange={e => setClip(Number(e.target.value))} /></label></div>
    <div className="viewer-help">Drag to orbit · Right-drag to pan · Scroll to zoom <span><b className="axis-x">X</b> <b className="axis-y">Y</b> <b className="axis-z">Z</b></span></div>
  </section>;
}

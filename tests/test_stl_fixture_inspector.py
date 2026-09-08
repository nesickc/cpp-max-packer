import json, struct, subprocess, sys, tempfile, unittest, importlib.util
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
_spec=importlib.util.spec_from_file_location('inspector',Path(__file__).parents[1]/'tools/Inspect-StlFixtures.py'); Inspect_StlFixtures=importlib.util.module_from_spec(_spec); _spec.loader.exec_module(Inspect_StlFixtures)

class InspectorTests(unittest.TestCase):
    def setUp(self): self.d = Path(tempfile.mkdtemp()); (self.d/'rc/items').mkdir(parents=True); (self.d/'rc/containers').mkdir()
    def tetra(self, name, binary=False, bad_count=None):
        vs=[(0,0,0),(1,0,0),(0,1,0),(0,0,1)]; ts=[(0,1,2),(0,3,1),(1,3,2),(0,2,3)]
        if binary:
            body=b''.join(struct.pack('<3f',0,0,0)+b''.join(struct.pack('<3f',*vs[i]) for i in t)+struct.pack('<H',0) for t in ts)
            n=len(ts) if bad_count is None else bad_count; data=b'solid tetra'+b'\0'*69+struct.pack('<I',n)+body
        else: data=('solid tetra\n'+''.join(' facet normal 0 0 0\n  outer loop\n'+''.join('   vertex %g %g %g\n'%v for v in [vs[i] for i in t])+'  endloop\n endfacet\n' for t in ts)+'endsolid tetra\n').encode()
        p=self.d/'rc/items'/name; p.write_bytes(data); return p
    def test_ascii_binary_and_solid_header(self):
        a=Inspect_StlFixtures.inspect(self.tetra('a.stl'),self.d); b=Inspect_StlFixtures.inspect(self.tetra('b.stl',True),self.d)
        self.assertEqual((a['triangles'],a['bounds_source_units']),(b['triangles'],b['bounds_source_units']))
        self.assertEqual(b['format'],'binary')
    def test_open_edge_and_closed_tetra(self):
        r=Inspect_StlFixtures.inspect(self.tetra('x.stl'),self.d); self.assertEqual(r['boundary_edges'],0)
        p=self.tetra('open.stl'); s=p.read_text(); s=s.replace(s[s.rfind(' facet'):s.rfind('endsolid')],''); p.write_text(s,encoding='ascii'); r=Inspect_StlFixtures.inspect(p,self.d); self.assertGreater(r['boundary_edges'],0)
    def test_bad_inputs_no_valid_claim(self):
        for name,data in [('trunc.stl',b'abc'),('bad.stl',b'solid x\nfacet nope\n'),('nan.stl',b'solid x\n facet normal 0 0 0\n outer loop\n vertex nan 0 0\n vertex 0 0 0\n vertex 0 1 0\n endloop\n endfacet\nendsolid')]:
            p=self.d/'rc/items'/name; p.write_bytes(data); r=Inspect_StlFixtures.inspect(p,self.d); self.assertEqual(r['format'],'unknown'); self.assertEqual(r['triangles'],0)
    def test_ascii_rejects_mixed_nonfinite_or_structurally_bad_faces(self):
        base=self.tetra('mixed.stl').read_text(); face=base[base.find(' facet'):base.find('endsolid')]
        bad=face.replace('vertex 0 0 0','vertex nan 0 0',1)
        for name, data in [('mixed-nan.stl',base.replace(face,face+bad)), ('missing-endfacet.stl',base.replace('endfacet\n','',1))]:
            p=self.d/'rc/items'/name; p.write_text(data); r=Inspect_StlFixtures.inspect(p,self.d); self.assertEqual(r['format'],'unknown'); self.assertFalse(r['finite_coordinates'])
    def test_ascii_parser_rejects_bad_vertex_without_partial_geometry(self):
        base=self.tetra('base.stl').read_text()
        bad=('facet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex oops 1 0\n'
             'vertex 0 1 0\nendloop\nendfacet\n')
        p=self.d/'rc/items'/'bad-vertex.stl'; p.write_text(base.replace('endsolid tetra\n',bad+'endsolid tetra\n'),encoding='ascii')
        r=Inspect_StlFixtures.inspect(p,self.d)
        self.assertEqual((r['format'],r['triangles']),('unknown',0))
        self.assertFalse(r['finite_coordinates'])

    def test_ascii_parser_rejects_complete_malformed_facet_without_partial_geometry(self):
        data=('solid sample\n'
              'facet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\n'
              'facet normal 0 0 1\nouter loop\nvertex oops 0 0\nvertex oops 0 0\nvertex oops 1 0\nendloop\nendfacet\n'
              'endsolid sample\n')
        p=self.d/'rc/items'/'malformed-token-face.stl'; p.write_text(data,encoding='ascii')
        r=Inspect_StlFixtures.inspect(p,self.d)
        self.assertEqual((r['format'],r['triangles']),('unknown',0))
        self.assertFalse(r['finite_coordinates'])
        self.assertTrue(r['parse_diagnostic'])

    def test_ascii_solid_names_are_free_text(self):
        for name in ('nan','facet'):
            data=self.tetra(name+'.stl').read_text().replace('solid tetra','solid '+name).replace('endsolid tetra','endsolid '+name)
            p=self.d/'rc/items'/(name+'.stl'); p.write_text(data,encoding='ascii')
            r=Inspect_StlFixtures.inspect(p,self.d)
            self.assertEqual((r['format'],r['triangles']),('ascii',4))
            self.assertTrue(r['finite_coordinates'])
    def test_binary_header_containing_facet_wins_exact_layout(self):
        p=self.d/'rc/items'/'binary-facet.stl'; body=struct.pack('<12fH',0,0,0,0,0,0,.5,0,0,0,.5,0,0); p.write_bytes(b'solid facet'.ljust(80,b'\0')+struct.pack('<I',1)+body)
        r=Inspect_StlFixtures.inspect(p,self.d); self.assertEqual((r['format'],r['triangles']),('binary',1)); self.assertTrue(r['finite_coordinates'])
    def test_unicode_and_immutability(self):
        p=self.tetra('тест.stl'); before=p.read_bytes(); Inspect_StlFixtures.inspect(p,self.d); self.assertEqual(before,p.read_bytes())
    def test_manifest_deterministic_and_pinned(self):
        root=Path(__file__).parents[1]; m=json.loads((root/'tests/fixtures/rc-manifest.json').read_text()); self.assertEqual(len(m['records']),10)
        with tempfile.TemporaryDirectory() as t:
            out=Path(t)/'m.json'; subprocess.check_call([sys.executable,str(root/'tools/Inspect-StlFixtures.py'),'--root',str(root),'--out',str(out),'--diagnostics',str(Path(t)/'d')]); self.assertEqual(json.loads(out.read_text()),m)

if __name__=='__main__': unittest.main()

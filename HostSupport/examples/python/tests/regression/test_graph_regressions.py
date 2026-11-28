"""Regression tests of ofxhost.graph and the file specs of its inputs."""

from pathlib import Path


def test_file_spec_paths(tmp_path, monkeypatch):
    """The alpha file of a "file:rgb.pfm+alpha.pfm@x,y#premult" spec is made absolute too, and names with '@',
    '#' and '+' are kept: resolve_file_spec() left the alpha file relative (review of #117-#123, cf8bbe4f, #126)."""
    from ofxhost import resolve_file_spec
    monkeypatch.chdir(tmp_path)
    assert resolve_file_spec("file:a.pfm+b.pfm@3,-4#premult") == \
        f"file:{tmp_path.resolve() / 'a.pfm'}+{tmp_path.resolve() / 'b.pfm'}@3,-4#premult"
    assert resolve_file_spec("file:sub/a.pfm", "/base") == "file:" + str(Path("/base/sub/a.pfm").resolve())
    # an alpha file with another extension, and '@', '#' and '+' in names
    assert resolve_file_spec("file:x@1.pfm+a#b.dat@0,0", "/base") == \
        "file:" + str(Path("/base/x@1.pfm").resolve()) + "+" + str(Path("/base/a#b.dat").resolve()) + "@0,0"

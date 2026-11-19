"""生成多进程源码测试包；不交叉编译、不连接开发板、不替换默认服务。"""
import argparse
from hashlib import sha256
import io
import json
from pathlib import Path
import subprocess
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--name', default='multiprocess-board-test-20261119')
    args = parser.parse_args()
    if not args.name or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in args.name):
        parser.error('--name 只允许小写字母、数字和连字符')
    root = Path(__file__).resolve().parents[1]
    files = {}
    for path in sorted((root / 'server').iterdir()):
        if path.suffix in ('.c', '.h') or path.name == 'Makefile':
            files[f'project/server/{path.name}'] = path.read_bytes()
    for source, target in (
        ('scripts/build-on-ubuntu.sh', 'build-on-ubuntu.sh'),
        ('scripts/start-server.sh', 'start-server.sh'),
        ('tests/board_tcp_check.py', 'board_tcp_check.py'),
        ('docs/process-board-test.md', 'README.md'),
        ('docs/architecture.md', 'architecture.md'),
    ):
        files[target] = (root / source).read_bytes()
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root, text=True).strip())
    manifest = {'revision': revision, 'working_tree_modified': dirty,
                'note': '源代码及构建入口，不包含 ARM 可执行文件；新进程版尚待板端验收。',
                'files': {name: sha256(content).hexdigest() for name, content in files.items()}}
    files['source-manifest.json'] = json.dumps(manifest, ensure_ascii=False, indent=2).encode('utf-8')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, 'w:gz') as archive:
        for name, content in files.items():
            info = tarfile.TarInfo(f'{args.name}/{name}')
            info.size = len(content)
            info.mode = 0o755 if name.endswith('.sh') else 0o644
            archive.addfile(info, io.BytesIO(content))
    # 读取成品，核对所有内容与摘要，避免漏文件或错误打包。
    with tarfile.open(args.output) as archive:
        assert len(archive.getmembers()) == len(files)
        for name, content in files.items():
            assert archive.extractfile(f'{args.name}/{name}').read() == content
    print(str(args.output.resolve()))
    print(f'SHA256 {sha256(args.output.read_bytes()).hexdigest()}')


if __name__ == '__main__':
    main()

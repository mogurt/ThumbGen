"""Ensure pressure evidence never silently falls back to Python OIIO."""
import importlib.abc
import sys


class BlockOIIO(importlib.abc.MetaPathFinder):
    def find_spec(self,fullname,path=None,target=None):
        if fullname.split('.',1)[0] in ('OpenImageIO','oiio'):
            raise ImportError('Python OIIO intentionally blocked for native-read-back pressure evidence')
        return None


sys.meta_path.insert(0,BlockOIIO())

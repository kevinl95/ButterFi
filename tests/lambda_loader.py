"""Load a Lambda's inline source (Code.ZipFile) from template.yaml as a module.

The Lambdas live inline in the CloudFormation template, so the tests import
exactly the code that deploys. boto3 is replaced by a stub (no AWS access);
tests swap in fakes for the table and the iotwireless client as needed.
"""
import importlib.util
import os
import sys
import types
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / 'template.yaml'


class _CfnLoader(yaml.SafeLoader):
    """SafeLoader that tolerates CloudFormation short tags (!Ref, !Sub, ...)."""


def _any_tag(loader, suffix, node):
    if isinstance(node, yaml.ScalarNode):
        return loader.construct_scalar(node)
    if isinstance(node, yaml.SequenceNode):
        return loader.construct_sequence(node)
    return loader.construct_mapping(node)


_CfnLoader.add_multi_constructor('!', _any_tag)


def lambda_source(logical_id):
    doc = yaml.load(TEMPLATE.read_text(), Loader=_CfnLoader)
    return doc['Resources'][logical_id]['Properties']['Code']['ZipFile']


class _StubBoto3(types.ModuleType):
    def __init__(self):
        super().__init__('boto3')

    def client(self, *args, **kwargs):
        return None

    def resource(self, *args, **kwargs):
        return None


def load_lambda(logical_id, env=None):
    """Import the Lambda's source as a fresh module (boto3 stubbed)."""
    defaults = {
        'CHUNK_TABLE': 'test-chunks',
        'POLICY_TABLE': '',
        'MAX_CHUNK_BYTES': '240',
        'CHUNK_TTL_HOURS': '24',
    }
    defaults.update(env or {})
    saved_env = {k: os.environ.get(k) for k in defaults}
    os.environ.update(defaults)
    saved_boto3 = sys.modules.get('boto3')
    sys.modules['boto3'] = _StubBoto3()
    try:
        name = f'lambda_{logical_id}'
        spec = importlib.util.spec_from_loader(name, loader=None)
        module = importlib.util.module_from_spec(spec)
        exec(compile(lambda_source(logical_id), f'<template.yaml:{logical_id}>', 'exec'),
             module.__dict__)
        return module
    finally:
        if saved_boto3 is None:
            sys.modules.pop('boto3', None)
        else:
            sys.modules['boto3'] = saved_boto3
        for k, v in saved_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

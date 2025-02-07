#-------------------------------------------------------------------------------
# Copyright (c) 2018-2022, Arm Limited. All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#
#-------------------------------------------------------------------------------

import os
import io
import re
import sys
import argparse
import logging
from jinja2 import Environment, BaseLoader, select_autoescape, TemplateNotFound

try:
    import yaml
except ImportError as e:
    logging.error (str(e) + " To install it, type:")
    logging.error ("pip install PyYAML")
    exit(1)

donotedit_warning = \
                    '  WARNING: This is an auto-generated file. Do not edit!  '

TFM_ROOT_DIR = os.path.join(sys.path[0], '..')
OUT_DIR = None # The root directory that files are generated to

# PID[0, TFM_PID_BASE - 1] are reserved for TF-M SPM and test usages
TFM_PID_BASE = 256

# variable for checking for duplicated sid
sid_list = []
class TemplateLoader(BaseLoader):
    """
    Template loader class.

    An instance of this class is passed to the template engine. It is
    responsible for reading the template file
    """
    def __init__(self):
        pass

    def get_source(self, environment, template):
        """
        This function reads the template files.
        For detailed documentation see:
        http://jinja.pocoo.org/docs/2.10/api/#jinja2.BaseLoader.get_source

        Please note that this function always return 'false' as 'uptodate'
        value, so the output file will always be generated.
        """
        if not os.path.isfile(template):
            raise TemplateNotFound(template)
        with open(template) as f:
            source = f.read()
        return source, template, False

def get_single_macro_def_from_file(file_name, macro_name):
    """
    This function parses the given file_name to get the definition of the given
    C Macro (macro_name).

    It assumes that the target Macro has no multiple definitions in different
    build configurations.

    It supports Macros defined in multi-line, for example:
    #define SOME_MACRO          \
                            the_macro_value

    Inputs:
        - file_name: the file to get the Macro from
        - macro_name: the name of the Macro to get
    Returns:
        - The Macro definition with '()' stripped, or Exception if not found
    """

    with open(file_name, 'r') as f:
        pattern = re.compile(r'#define\s+{}[\\\s]+.*'.format(macro_name))
        result = pattern.findall(f.read())

        if len(result) != 1:
            raise Exception('{} not defined or has multiple definitions'.format(macro_name))

    macro_def = result[0].split()[-1].strip('()')

    return macro_def

def manifest_validation(partition_manifest, pid):
    """
    This function validates FF-M compliance for partition manifest, and sets
    default values for optional attributes.
    The validation is skipped for TF-M specific Partitions (PID < TFM_PID_BASE).
    More validation items will be added.
    """

    service_list = partition_manifest.get('services', [])
    irq_list     = partition_manifest.get('irqs', [])

    if (pid == None or pid >= TFM_PID_BASE) \
       and len(service_list) == 0 and len(irq_list) == 0:
        raise Exception('{} must declare at least either a secure service or an IRQ!'
                        .format(partition_manifest['name']))

    # Service FF-M manifest validation
    for service in service_list:
        if 'version' not in service.keys():
            service['version'] = 1
        if 'version_policy' not in service.keys():
            service['version_policy'] = 'STRICT'

        # SID duplication check
        if service['sid'] in sid_list:
            raise Exception('Service ID: {} has duplications!'.format(service['sid']))
        else:
            sid_list.append(service['sid'])

    return partition_manifest

def process_partition_manifests(manifest_lists, isolation_level, backend):
    """
    Parse the input manifest lists, generate the data base for genereated files
    and generate manifest header files.

    Parameters
    ----------
    manifest_lists:
        A list of Secure Partition manifest lists and their original paths.
        The manifest lists might be processed by CMake and the paths might be
        different to the original ones. Original paths are needed to handle
        relative paths in the lists.
        The format must be [list A, orignal path A, list B, orignal path B, ...]

    Returns
    -------
    The manifest data base.
    """

    context = {}

    partition_list = []
    all_manifests = []
    pid_list = []
    no_pid_manifest_idx = []
    partition_statistics = {
        'connection_based_srv_num': 0,
        'ipc_partition_num': 0,
        'sfn_partition_num': 0,
        'flih_num': 0,
        'slih_num': 0
    }
    config_impl = {
        'CONFIG_TFM_SPM_BACKEND_SFN'              : '0',
        'CONFIG_TFM_SPM_BACKEND_IPC'              : '0',
        'CONFIG_TFM_PSA_API_SFN_CALL'             : '0',
        'CONFIG_TFM_PSA_API_CROSS_CALL'           : '0',
        'CONFIG_TFM_PSA_API_SUPERVISOR_CALL'      : '0',
        'CONFIG_TFM_CONNECTION_BASED_SERVICE_API' : '0',
        'CONFIG_TFM_FLIH_API'                     : '0',
        'CONFIG_TFM_SLIH_API'                     : '0'
    }

    # Get all the manifests information as a dictionary
    for i, item in enumerate(manifest_lists):
        if i % 2 == 0 and not os.path.isfile(item):
            logging.error('Manifest list item [{}] must be a file'.format(i))
            exit(1)

        if i % 2 == 1:
            if not os.path.isdir(item):
                logging.error('Manifest list item [{}] must be a directory'.format(i))
                exit(1)

            # Skip original manifest paths
            continue

        # The manifest list file generated by configure_file()
        with open(item) as manifest_list_yaml_file:
            manifest_dic = yaml.safe_load(manifest_list_yaml_file)['manifest_list']
            for dict in manifest_dic:
                # Add original path of manifest list.
                # The validation will be done in the next loop.
                dict['list_path'] = manifest_lists[i + 1]
                all_manifests.append(dict)

    # Parse the manifests
    for i, manifest_item in enumerate(all_manifests):
        valid_enabled_conditions  = ['on',  'true',  'enabled']
        valid_disabled_conditions = ['off', 'false', 'disabled', '']
        is_enabled = ''

        if 'conditional' in manifest_item.keys():
            is_enabled = str(manifest_item['conditional']).lower()
        else:
            # Partitions without 'conditional' is alwasy on
            is_enabled = 'on'

        if is_enabled in valid_disabled_conditions:
            continue
        elif is_enabled not in valid_enabled_conditions:
            raise Exception('Invalid "conditional" attribute: "{}" for {}. '
                            'Please set to one of {} or {}, case-insensitive.'\
                            .format(manifest_item['conditional'],
                                    manifest_item['name'],
                                    valid_enabled_conditions, valid_disabled_conditions))

        # Check if partition ID is manually set
        if 'pid' not in manifest_item.keys():
            no_pid_manifest_idx.append(i)
            pid = None
        else:
            pid = manifest_item['pid']

            # Check if partition ID is duplicated
            if pid in pid_list:
                raise Exception('PID No. {pid} has already been used!'.format(pid))
            else:
                pid_list.append(pid)

        # Replace environment variables in the manifest path
        manifest_path = os.path.expandvars(manifest_item['manifest'])
        # Convert to absolute path. If it's already abspath, the path will not be changed.
        manifest_path = os.path.join(manifest_item['list_path'], manifest_path).replace('\\', '/')

        with open(manifest_path) as manifest_file:
            manifest = manifest_validation(yaml.safe_load(manifest_file), pid)

        if pid == None or pid >= TFM_PID_BASE:
            # Count the number of IPC/SFN partitions
            if manifest['psa_framework_version'] == 1.1 and manifest['model'] == 'SFN':
                partition_statistics['sfn_partition_num'] += 1
            else:
                partition_statistics['ipc_partition_num'] += 1

        for service in manifest.get('services', []):
            if 'connection_based' not in service.keys():
                partition_statistics['connection_based_srv_num'] += 1
            elif service['connection_based']:
                partition_statistics['connection_based_srv_num'] += 1

        for irq in manifest.get('irqs', []):
            if irq.get('handling', None) == 'FLIH':
                partition_statistics['flih_num'] += 1
            else:
                partition_statistics['slih_num'] += 1

        manifest_out_basename = os.path.splitext(os.path.basename(manifest_path))[0]

        if 'output_path' in manifest_item:
            output_path = os.path.expandvars(manifest_item['output_path'])
        else:
            output_path = ''

        manifest_head_file = os.path.join(OUT_DIR, output_path, 'psa_manifest',
                                          '{}.h'.format(manifest_out_basename))\
                                              .replace('\\', '/')
        intermedia_file    = os.path.join(OUT_DIR, output_path, 'auto_generated',
                                          'intermedia_{}.c'.format(manifest_out_basename))\
                                              .replace('\\', '/')
        load_info_file     = os.path.join(OUT_DIR, output_path, 'auto_generated',
                                          'load_info_{}.c'.format(manifest_out_basename))\
                                              .replace('\\', '/')

        partition_list.append({'manifest': manifest, 'attr': manifest_item,
                               'manifest_out_basename': manifest_out_basename,
                               'header_file': manifest_head_file,
                               'intermedia_file': intermedia_file,
                               'loadinfo_file': load_info_file})

    # Automatically assign PIDs for partitions without 'pid' attribute
    pid = max(pid_list, default = TFM_PID_BASE - 1)
    for idx in no_pid_manifest_idx:
        pid += 1
        all_manifests[idx]['pid'] = pid
        pid_list.append(pid)

    # Set up configurations
    if backend == 'SFN':
        if partition_statistics['ipc_partition_num'] > 0:
            logging.error('SFN backend does not support IPC Partitions.')
            exit(1)

        if isolation_level > 1:
            logging.error('SFN backend does not support high isolation levels.')
            exit(1)

        config_impl['CONFIG_TFM_SPM_BACKEND_SFN'] = '1'
        config_impl['CONFIG_TFM_PSA_API_SFN_CALL'] = '1'
    elif backend == 'IPC':
        if partition_statistics['sfn_partition_num'] > 0:
            logging.error('IPC backend does not support SFN Partitions.')
            exit(1)

        config_impl['CONFIG_TFM_SPM_BACKEND_IPC'] = '1'
        if isolation_level > 1:
            config_impl['CONFIG_TFM_PSA_API_SUPERVISOR_CALL'] = '1'
        else:
            config_impl['CONFIG_TFM_PSA_API_CROSS_CALL'] = '1'

    if partition_statistics['connection_based_srv_num'] > 0:
        config_impl['CONFIG_TFM_CONNECTION_BASED_SERVICE_API'] = 1

    if partition_statistics['flih_num'] > 0:
        config_impl['CONFIG_TFM_FLIH_API'] = 1
    elif partition_statistics['slih_num'] > 0:
        config_impl['CONFIG_TFM_SLIH_API'] = 1

    context['partitions'] = partition_list
    context['config_impl'] = config_impl
    context['stateless_services'] = process_stateless_services(partition_list)

    return context

def gen_per_partition_files(context):
    """
    Generate per-partition files

    Parameters
    ----------
    context:
        context contains partition infos
    """

    partition_context = {}
    partition_context['utilities'] = context['utilities']

    manifesttemplate = ENV.get_template(os.path.join(sys.path[0], 'templates/manifestfilename.template'))
    memorytemplate = ENV.get_template(os.path.join(sys.path[0], 'templates/partition_intermedia.template'))
    infotemplate = ENV.get_template(os.path.join(sys.path[0], 'templates/partition_load_info.template'))

    logging.info ("Start to generate partition files:")

    for one_partition in context['partitions']:
        partition_context['manifest'] = one_partition['manifest']
        partition_context['attr'] = one_partition['attr']
        partition_context['manifest_out_basename'] = one_partition['manifest_out_basename']

        logging.info ("Generating Header: " + one_partition['header_file'])
        outfile_path = os.path.dirname(one_partition['header_file'])
        if not os.path.exists(outfile_path):
            os.makedirs(outfile_path)

        headerfile = io.open(one_partition['header_file'], 'w', newline=None)
        headerfile.write(manifesttemplate.render(partition_context))
        headerfile.close()

        logging.info ("Generating Intermedia: " + one_partition['intermedia_file'])
        intermediafile_path = os.path.dirname(one_partition['intermedia_file'])
        if not os.path.exists(intermediafile_path):
            os.makedirs(intermediafile_path)
        intermediafile = io.open(one_partition['intermedia_file'], 'w', newline=None)
        intermediafile.write(memorytemplate.render(partition_context))
        intermediafile.close()

        logging.info ("Generating Loadinfo: " + one_partition['loadinfo_file'])
        infofile_path = os.path.dirname(one_partition['loadinfo_file'])
        if not os.path.exists(infofile_path):
            os.makedirs(infofile_path)
        infooutfile = io.open(one_partition['loadinfo_file'], 'w', newline=None)
        infooutfile.write(infotemplate.render(partition_context))
        infooutfile.close()

    logging.info ("Per-partition files done:")

def gen_summary_files(context, gen_file_lists):
    """
    Generate files according to the gen_file_list

    Parameters
    ----------
    gen_file_lists:
        The lists of files to generate
    """
    file_list = []

    for f in gen_file_lists:
        with open(f) as file_list_yaml_file:
            file_list_yaml = yaml.safe_load(file_list_yaml_file)
            file_list.extend(file_list_yaml['file_list'])

    logging.info("Start to generate file from the generated list:")
    for file in file_list:
        # Replace environment variables in the output filepath
        manifest_out_file = os.path.expandvars(file['output'])
        # Replace environment variables in the template filepath
        templatefile_name = os.path.expandvars(file['template'])

        manifest_out_file = os.path.join(OUT_DIR, manifest_out_file)

        logging.info ("Generating " + manifest_out_file)

        outfile_path = os.path.dirname(manifest_out_file)
        if not os.path.exists(outfile_path):
            os.makedirs(outfile_path)

        template = ENV.get_template(templatefile_name)

        outfile = io.open(manifest_out_file, 'w', newline=None)
        outfile.write(template.render(context))
        outfile.close()

    logging.info ("Generation of files done")

def process_stateless_services(partitions):
    """
    This function collects all stateless services together, and allocates
    stateless handles for them.
    Valid stateless handle in service will be converted to an index. If the
    stateless handle is set as "auto", or not set, framework will allocate a
    valid index for the service.
    Framework puts each service into a reordered stateless service list at
    position of "index". Other unused positions are left None.
    """

    STATIC_HANDLE_CONFIG_FILE = 'secure_fw/spm/cmsis_psa/spm_ipc.h'

    collected_stateless_services = []
    stateless_index_max_num = \
        int(get_single_macro_def_from_file(STATIC_HANDLE_CONFIG_FILE, 'STATIC_HANDLE_NUM_LIMIT'), base = 10)

    # Collect all stateless services first.
    for partition in partitions:
        # Skip the FF-M 1.0 partitions
        if partition['manifest']['psa_framework_version'] < 1.1:
            continue

        service_list = partition['manifest'].get('services', [])

        for service in service_list:
            if 'connection_based' not in service:
                raise Exception("'connection_based' is mandatory in FF-M 1.1 service!")
            if service['connection_based'] is False:
                collected_stateless_services.append(service)

    if len(collected_stateless_services) == 0:
        return []

    if len(collected_stateless_services) > stateless_index_max_num:
        raise Exception('Stateless service numbers range exceed {number}.'.format(number=stateless_index_max_num))

    """
    Allocate an empty stateless service list to store services.
    Use "handle - 1" as the index for service, since handle value starts from
    1 and list index starts from 0.
    """
    reordered_stateless_services = [None] * stateless_index_max_num
    auto_alloc_services = []

    for service in collected_stateless_services:
        # If not set, it is "auto" by default
        if 'stateless_handle' not in service:
            auto_alloc_services.append(service)
            continue

        service_handle = service['stateless_handle']

        # Fill in service list with specified stateless handle, otherwise skip
        if isinstance(service_handle, int):
            if service_handle < 1 or service_handle > stateless_index_max_num:
                raise Exception('Invalid stateless_handle setting: {handle}.'.format(handle=service['stateless_handle']))
            # Convert handle index to reordered service list index
            service_handle = service_handle - 1

            if reordered_stateless_services[service_handle] is not None:
                raise Exception('Duplicated stateless_handle setting: {handle}.'.format(handle=service['stateless_handle']))
            reordered_stateless_services[service_handle] = service
        elif service_handle == 'auto':
            auto_alloc_services.append(service)
        else:
            raise Exception('Invalid stateless_handle setting: {handle}.'.format(handle=service['stateless_handle']))

    STATIC_HANDLE_IDX_BIT_WIDTH = \
        int(get_single_macro_def_from_file(STATIC_HANDLE_CONFIG_FILE, 'STATIC_HANDLE_IDX_BIT_WIDTH'), base = 10)
    STATIC_HANDLE_IDX_MASK = (1 << STATIC_HANDLE_IDX_BIT_WIDTH) - 1

    STATIC_HANDLE_INDICATOR_OFFSET = \
        int(get_single_macro_def_from_file(STATIC_HANDLE_CONFIG_FILE, 'STATIC_HANDLE_INDICATOR_OFFSET'), base = 10)

    STATIC_HANDLE_VER_OFFSET = \
        int(get_single_macro_def_from_file(STATIC_HANDLE_CONFIG_FILE, 'STATIC_HANDLE_VER_OFFSET'), base = 10)

    STATIC_HANDLE_VER_BIT_WIDTH = \
        int(get_single_macro_def_from_file(STATIC_HANDLE_CONFIG_FILE, 'STATIC_HANDLE_VER_BIT_WIDTH'), base = 10)
    STATIC_HANDLE_VER_MASK = (1 << STATIC_HANDLE_VER_BIT_WIDTH) - 1

    # Auto-allocate stateless handle and encode the stateless handle
    for i in range(0, stateless_index_max_num):
        service = reordered_stateless_services[i]

        if service == None and len(auto_alloc_services) > 0:
            service = auto_alloc_services.pop(0)

        """
        Encode stateless flag and version into stateless handle
        Check STATIC_HANDLE_CONFIG_FILE for details
        """
        stateless_handle_value = 0
        if service != None:
            stateless_index = (i & STATIC_HANDLE_IDX_MASK)
            stateless_handle_value |= stateless_index
            stateless_handle_value |= (1 << STATIC_HANDLE_INDICATOR_OFFSET)
            stateless_version = (service['version'] & STATIC_HANDLE_VER_MASK) << STATIC_HANDLE_VER_OFFSET
            stateless_handle_value |= stateless_version
            service['stateless_handle_value'] = '0x{0:08x}'.format(stateless_handle_value)
            service['stateless_handle_index'] = stateless_index

        reordered_stateless_services[i] = service

    return reordered_stateless_services

def parse_args():
    parser = argparse.ArgumentParser(description='Parse secure partition manifest list and generate files listed by the file list',
                                     epilog='Note that environment variables in template files will be replaced with their values')

    parser.add_argument('-o', '--outdir'
                        , dest='outdir'
                        , required=True
                        , metavar='out_dir'
                        , help='The root directory for generated files')

    parser.add_argument('-m', '--manifest-lists'
                        , nargs='+'
                        , dest='manifest_lists'
                        , required=True
                        , metavar='manifest list'
                        , help='A list of Secure Partition manifest lists and their original paths.\n\
                                The manifest lists might be processed by CMake and\n\
                                the path might be different to the original one\n\
                                The format must be [list A, orignal path A, list B, orignal path B, ...]')

    parser.add_argument('-f', '--file-list'
                        , nargs='+'
                        , dest='gen_file_args'
                        , required=True
                        , metavar='file-list'
                        , help='These files descripe the file list to generate')

    parser.add_argument('-l', '--isolation-level'
                        , dest='isolation_level'
                        , required=True
                        , choices=['1', '2', '3']
                        , metavar='isolation-level')

    parser.add_argument('-b', '--backend'
                        , dest='backend'
                        , required=True
                        , choices=['IPC', 'SFN']
                        , metavar='spm-backend'
                        , help='The isolation level')

    parser.add_argument('-q', '--quiet'
                        , dest='quiet'
                        , required=False
                        , default=False
                        , action='store_true'
                        , help='Reduce log messages')

    args = parser.parse_args()

    if len(args.manifest_lists) % 2 != 0:
        logging.error('Invalid structure in manifest lists.\n'
              'Each element shall consist of a manifest list and its original path')
        exit(1)

    return args

ENV = Environment(
        loader = TemplateLoader(),
        autoescape = select_autoescape(['html', 'xml']),
        lstrip_blocks = True,
        trim_blocks = True,
        keep_trailing_newline = True
    )

def main():
    """
    The entry point of the script.

    Generates the output files based on the templates and the manifests.
    """

    global OUT_DIR

    args = parse_args()

    logging.basicConfig(format='%(message)s'
                        , level=logging.WARNING if args.quiet else logging.INFO)

    OUT_DIR = os.path.abspath(args.outdir)

    manifest_lists = [os.path.abspath(x) for x in args.manifest_lists]
    gen_file_lists = [os.path.abspath(x) for x in args.gen_file_args]

    """
    Relative path to TF-M root folder is supported in the manifests
    and default value of manifest list and generated file list are relative to TF-M root folder as well,
    so first change directory to TF-M root folder.
    By doing this, the script can be executed anywhere
    The script is located in <TF-M root folder>/tools, so sys.path[0]<location of the script>/.. is TF-M root folder.
    """
    os.chdir(os.path.join(sys.path[0], '..'))

    context = process_partition_manifests(manifest_lists,
                                          int(args.isolation_level),
                                          args.backend)

    utilities = {}
    utilities['donotedit_warning'] = donotedit_warning

    context['utilities'] = utilities

    gen_per_partition_files(context)
    gen_summary_files(context, gen_file_lists)

if __name__ == '__main__':
    main()

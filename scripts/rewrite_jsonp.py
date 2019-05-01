from argparse import ArgumentParser
from tempfile import NamedTemporaryFile

import random
import subprocess
import os

# Decode file
def Decode(input_filename, encoding, file_id):
    if encoding == 'gzip' or encoding == 'deflate':
        command = 'gzip -df {0}'.format(input_filename)
    elif encoding == 'br':
        command = 'brotli -df {0}'.format(input_filename)
    else:
        return input_filename
    subprocess.call(command, shell=True)
    # remove extension after decompressing
    return input_filename[:len(input_filename) - 3]


def Encode(output, encoding, file_id):
    with NamedTemporaryFile() as temp:
        temp.write(output)
        temp.flush()

        output_filename = 'rewritten.' + file_id 
        if encoding == 'gzip' or encoding == 'deflate':
            command = 'gzip -c {0} > {1}'.format(temp.name, output_filename)
        elif encoding == 'br':
            command = 'brotli -c {0} > {1}'.format(temp.name, output_filename)
        else:
            command = 'cat {0} > {1}'.format(temp.name, output_filename)
        subprocess.call(command, shell=True)
        return output_filename


def RewriteFn(decoded_fn_call_filename, callback_fn):
    with open(decoded_fn_call_filename, 'r') as decoded_fn_call_f:
        decoded_fn_call = decoded_fn_call_f.read()
        open_paren_index = decoded_fn_call.find('(')
        return callback_fn + decoded_fn_call[open_paren_index:]


def Main():
    file_id = str(random.random())
    decoded_filename = Decode(args.encoded_input_filename, args.encoding, file_id)
    rewritten = RewriteFn(decoded_filename, args.callback_fn)
    output_filename = Encode(rewritten, args.encoding, file_id)
    if os.path.exists(decoded_filename):
        os.remove(decoded_filename)
    print(output_filename)

if __name__ == '__main__':
    parser = ArgumentParser()
    parser.add_argument('encoded_input_filename')
    parser.add_argument('callback_fn')
    parser.add_argument('encoding')
    args = parser.parse_args()
    Main()

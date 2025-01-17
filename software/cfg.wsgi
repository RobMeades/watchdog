#!/usr/bin/env python

# Note: this is actually a Python script but needs to have the extension
# .wsgi as, if it has a .py extension, Apache, which is being told to invoke
# this file via the Apache WSGI script module, treats the file as a static
# file and not a script.

# Update watchdog.cfg as a result of an HTTP POST request
# and server watchdog.cfg as a result of an HTTP GET request.

from os import path
import logging

# The file name to GET/POST (noting that the code assumes
# this file is in the same directory as this file)
FILE_NAME = 'watchdog.cfg'

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

def application(environ, start_response):
    file_path = path.join(path.dirname(path.realpath(__file__)), FILE_NAME)

    try:
        if environ['REQUEST_METHOD'] == 'GET':
            if path.exists(file_path):
                start_response('200 OK', [('Content-Type', 'application/json')])
                with open(file_path, 'r') as f:
                    json_data = f.read()
                return [str.encode(json_data)]
            else:
                start_response('404 File Not Found', [('Content-Type', 'text/plain')])
                return [b'File Not Found.']

        elif environ['REQUEST_METHOD'] == 'POST':
            try:
                content_length = int(environ.get('CONTENT_LENGTH', 0))
            except ValueError:
                start_response('400 Bad Request', [('Content-Type', 'text/plain')])
                return [b'Invalid Content-Length']

            post_data = environ['wsgi.input'].read(content_length)
            try:
                with open(file_path, 'w') as f:
                    f.write(post_data.decode('utf-8'))
                start_response('200 OK', [('Content-Type', 'text/plain')])
                return [b'File uploaded successfully!']
            except IOError as e:
                logger.error(f"Failed to write file: {e}")
                start_response('500 Internal Server Error', [('Content-Type', 'text/plain')])
                return [b'Failed to write file.']

        else:
            start_response('405 Method Not Allowed', [('Content-Type', 'text/plain')])
            return [b'Method Not Allowed.']

    except Exception as e:
        logger.error(f"Unexpected error: {e}")
        start_response('500 Internal Server Error', [('Content-Type', 'text/plain')])
        return [b'Internal Server Error.']
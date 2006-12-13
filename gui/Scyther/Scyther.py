#!/usr/bin/python
#
# Scyther interface
#

#---------------------------------------------------------------------------

""" Import externals """
import os
import os.path
import sys
import StringIO
import tempfile

#---------------------------------------------------------------------------

""" Import scyther components """
import XMLReader
from Misc import *

#---------------------------------------------------------------------------

"""
The default path for the binaries is set in __init__.py in the (current)
directory 'Scyther'.
"""

def setBinDir(dir):
    global bindir

    bindir = dir

def getBinDir():
    global bindir

    return bindir

#---------------------------------------------------------------------------

def getScytherBackend():
    # Where is my executable?
    #
    # Auto-detect platform and infer executable name from that
    #
    if "linux" in sys.platform:

        """ linux """
        scythername = "scyther-linux"

    elif "darwin" in sys.platform:

        """ OS X """
        # Preferably, we test for architecture (PPC/Intel) until we
        # know how to build a universal binary
        scythername = "scyther-osx"

    elif sys.platform.startswith('win'):

        """ Windows """
        scythername = "scyther-w32.exe"

    else:

        """ Unsupported"""
        print "ERROR: I'm sorry, the %s platform is unsupported at the moment" % (sys.platform)
        sys.exit()

    program = os.path.join(getBinDir(),scythername)
    if not os.path.isfile(program):
        print "I can't find the Scyther executable at %s" % (program)
        return None

    return program

#---------------------------------------------------------------------------

class Scyther(object):
    def __init__ ( self):

        # Init
        self.program = getScytherBackend()
        self.spdl = None
        self.inputfile = None
        self.options = ""
        self.claims = None
        self.errors = None
        self.errorcount = 0
        self.run = False
        self.output = None

        # defaults
        self.xml = True     # this results in a claim end, otherwise we simply get the output

    def setInput(self,spdl):
        self.spdl = spdl
        self.inputfile = None

    def setFile(self,filename):
        self.inputfile = filename
        self.spdl = ""
        fp = open(filename,"r")
        for l in fp.readlines():
            self.spdl += l
        fp.close()

    def addFile(self,filename):
        self.inputfile = None
        if not self.spdl:
            self.spdl = ""
        fp = open(filename,"r")
        for l in fp.readlines():
            self.spdl += l
        fp.close()

    def verify(self):
        """ Should return a list of results """

        if self.program == None:
            return []

        # Generate a temproary file for the error output
        errorfile = tempfile.NamedTemporaryFile()

        # Generate command line for the Scyther process
        self.cmd = "\"%s\"" % self.program
        if self.xml:
            self.cmd += " --dot-output --xml-output --plain"
        self.cmd += " --errors=%s" % (errorfile.name)
        self.cmd += " " + self.options

        # Start the process, push input, get output
        (stdin,stdout) = os.popen2(self.cmd)
        if self.spdl:
            stdin.write(self.spdl)
        stdin.close()
        output = stdout.read()

        # get errors
        # filter out any non-errors (say maybe only claim etc) and count
        # them.
        self.errors = []
        errorfile.seek(0)
        for l in errorfile.readlines():
            if not l.startswith("claim\t"):
                self.errors.append(l.strip())

        self.errorcount = len(self.errors)
        
        # close
        stdout.close()
        errorfile.close()

        if self.xml:
            self.validxml = False
            if len(output) > 0:
                if output.startswith("<scyther>"):
                    self.validxml = True

            if self.validxml:
                xmlfile = StringIO.StringIO(output)
                reader = XMLReader.XMLReader()
                self.claims = reader.readXML(xmlfile)
            else:
                # no xml output... store whatever comes out
                self.claims = []
                self.output = output
            result = self.claims
        else:
            self.output = output
            result = self.output

        self.run = True
        return result

    def getClaim(self,claimid):
        if self.claims:
            for cl in self.claims:
                if cl.id == claimid:
                    return cl
        return None

    def __str__(self):
        if self.run:
            if self.errorcount > 0:
                return "%i errors:\n%s" % (self.errorcount, "\n".join(self.errors))
            else:
                if self.xml and self.validxml:
                    s = "Verification results:\n"
                    for cl in self.claims:
                        s += str(cl) + "\n"
                    return s
                else:
                    return self.output
        else:
            return "Scyther has not been run yet."


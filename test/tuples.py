#!/usr/bin/python
#
# Given a number of input lines on std and an argument int, this program
# generates unordered tuples, e.g.:
#
# arg:	2
# in:	a
# 	b
# 	c
# 	d
#
# out:	a,b
# 	a,c
# 	a,d
# 	b,c
# 	b,d
# 	c,d
#
# This should make it clear what happens.
#
import sys
import string
import tuplesdo

# Retrieve the tuple width
tuplesize = int(sys.argv[1])

# Read stdin into list and count
list = []
loop = 1
while loop:
	line = sys.stdin.readline()
	if line != '':
		# not the end of the input
		line = string.strip(line)
		if line != '':
			# not a blank line
			list.append(line)
	else:
		# end of the input
		loop = 0

def tuplesPrint (l, n):
	def f (resultlist):
		print " ".join(resultlist)

	tuplesdo.tuplesDo (f, l, n)

# Generate tuples...
tuplesPrint (list, tuplesize)
# Thanks for your attention
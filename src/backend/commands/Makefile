#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/commands
#
# IDENTIFICATION
#    src/backend/commands/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/commands
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	mys_createas.o \
	mys_ddl_helpers.o \
	mys_uservar.o \
	aggregatecmds.o \
	alter.o \
	amcmds.o \
	analyze.o \
	async.o \
	cluster.o \
	collationcmds.o \
	comment.o \
	constraint.o \
	conversioncmds.o \
	copy.o \
	copyfrom.o \
	copyfromparse.o \
	copyto.o \
	createas.o \
	dbcommands.o \
	define.o \
	discard.o \
	dropcmds.o \
	event_trigger.o \
	explain.o \
	explain_dr.o \
	explain_format.o \
	explain_state.o \
	extension.o \
	foreigncmds.o \
	functioncmds.o \
	indexcmds.o \
	lockcmds.o \
	matview.o \
	opclasscmds.o \
	operatorcmds.o \
	policy.o \
	portalcmds.o \
	prepare.o \
	proclang.o \
	publicationcmds.o \
	schemacmds.o \
	seclabel.o \
	sequence.o \
	statscmds.o \
	subscriptioncmds.o \
	tablecmds.o \
	tablespace.o \
	trigger.o \
	tsearchcmds.o \
	typecmds.o \
	user.o \
	vacuum.o \
	vacuumparallel.o \
	variable.o \
	view.o

include $(top_srcdir)/src/backend/common.mk



# Use the standard compilation rule, including header dependency tracking.
vpath mys_%.c $(srcdir)/mysql

/*!\file modelchecker.c
 * \brief The main procedures guiding the (optimized) traversal of the state space.
 *
 * This file implements various traversal methods through the state space.
 */

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include "substitutions.h"
#include "knowledge.h"
#include "runs.h"
#include "debug.h"
#include "modelchecker.h"
#include "report.h"
#include "memory.h"
#include "match_basic.h"
#include "match_clp.h"
#include "output.h"
#include "tracebuf.h"
#include "attackminimize.h"
#include "claims.h"

/*

	A model checker. Really.
*/

extern Term CLAIM_Secret;
extern Term CLAIM_Nisynch;

/*
   Some forward declarations.
*/

__inline__ int traverseSimple (const System oldsys);
__inline__ int traverseNonReads (const System oldsys);
__inline__ int traversePOR (const System oldsys);
__inline__ int traversePOR2 (const System oldsys);
__inline__ int traversePOR2b (const System oldsys);
__inline__ int traversePOR3 (const System oldsys);
__inline__ int traversePOR4 (const System oldsys);
__inline__ int traversePOR5 (const System oldsys);
__inline__ int traversePOR6 (const System oldsys);
__inline__ int traversePOR7 (const System oldsys);
__inline__ int traversePOR8 (const System oldsys);
int propertyCheck (const System sys);
int executeTry (const System sys, int run);
int claimSecrecy (const System sys, const Term t);
int violateClaim (const System sys, int length, int claimev, Termlist reqt);
Termlist secrecyUnfolding (Term t, const Knowledge know);

/*
   Main code.
*/

void
statePrint (const System sys)
{
  int i, s;
  Roledef rd;

  indent ();
  printf ("state %i: ", sys->step);
  for (i = 0; i < sys->maxruns; i++)
    {
      s = 0;
      rd = runPointerGet (sys, i);
      while (rd != NULL)
	{
	  rd = rd->next;
	  s++;
	}
      printf ("%i ", s);
    }
  printf (" - phase %i, done %i", sys->PORphase, sys->PORdone);
  printf ("\n");
}

int
traverse (const System sys)
{
  /* branch for traversal methods */
  switch (sys->traverse)
    {
    case 1:
      return traverseSimple (sys);
    case 2:
      return traverseNonReads (sys);
    case 3:
    case 4:
      return traversePOR (sys);
    case 5:
      return traversePOR2 (sys);
    case 6:
      return traversePOR3 (sys);
    case 7:
      return traversePOR2b (sys);
    case 8:
      return traversePOR4 (sys);
    case 9:
      return traversePOR5 (sys);
    case 10:
      return traversePOR6 (sys);
    case 11:
      return traversePOR7 (sys);
    case 12:
      return traversePOR8 (sys);
    default:
      debug (2, "This is NOT an existing traversal method !");
      exit (1);
    }
}

//! Progress counters to next step.
/**
 * Does not really execute anything, it's just bookkeeping, progressing
 * counters and such.
 *
 *@returns If it returns TRUE, explore. If false, don't traverse.
 */

int
executeStep (const System sys, const int run)
{
  Roledef runPoint;
  runPoint = runPointerGet (sys, run);
#ifdef DEBUG
  if (DEBUGL (3))
    {
      indent ();
      printf ("exec: ");
      roledefPrint (runPoint);
      printf ("#%i\n", run);
    }
#endif
  sys->runs[run].step++;
  runPointerSet (sys, run, runPoint->next);

  /* store knowledge for this step */
  (sys->step)++;
  sys->traceKnow[sys->step] = sys->know;

  /* check for properties */
  propertyCheck (sys);

  /* set indent for printing */
  indentSet (sys->step);
  /* hmmm, but what should it return if not exploring? */
  if (!sys->explore)
    return 0;

  /* we want to explore it, but are we allowed by pruning? */
  if (sys->step >= sys->maxtracelength)
    {
      /* cut off traces that are too long */
#ifdef DEBUG
      if (DEBUGL (4))
	{
	  indent ();
	  printf ("trace cut off.\n");
	  if (DEBUGL (5))
	    {
	      (sys->step)--;
	      tracePrint (sys);
	      (sys->step)++;
	    }
	}
#endif
      return 0;
    }

  /* we will explore this state, so count it. */
  sys->states = statesIncrease (sys->states);

  /* show progression */
  if (sys->switchS > 0)
    {
      sys->interval = statesIncrease (sys->interval);
      if (!statesSmallerThan (sys->interval, (unsigned long int) sys->switchS))
	{
	  sys->interval = STATES0;
	  fprintf (stderr, "States ");
	  statesFormat (stderr, sys->states);
	  fprintf (stderr, " \r");
	}
    }

  /* store new node numbder */
  sys->traceNode[sys->step] = sys->states;
  /* the construction below always assumes MAX_GRAPH_STATES to be smaller than the unsigned long it, which seems realistic. */
  if (sys->switchStatespace && statesSmallerThan (sys->states, MAX_GRAPH_STATES))
    {
      /* display graph */
      graphNode (sys);
    }
  return 1;
}

/**
 * Determine for a roledef that is instantiated, the uninteresting ends bits.
 *
 *@todo "What is interesting" relies on the fact that there are only secrecy, sychnr and agreement properties.
 */
Roledef removeIrrelevant (const System sys, const int run, Roledef rd)
{
  Roledef rdkill;
  int killclaims;
  
  if (untrustedAgent (sys, sys->runs[run].agents))
      killclaims = 1;
  else
      killclaims = 0;

  rdkill = rd;
  while (rd != NULL)
    {
      if (rd->type == SEND || (!killclaims && rd->type == CLAIM))
	  rdkill = rd;
      rd = rd->next;
    }
  /* report part */
  /*
  rd = rdkill->next;
  killclaims = 0;
  while (rd != NULL)
    {
      killclaims++;
      rd = rd->next;
    }
  if (killclaims > 1)
    {
      warning ("%i events stripped from run %i.", killclaims, run);
      runPrint (rdkill->next);
    }
  */

  /* remove after rdkill */
  return rdkill;
}

//! Explores the system state given by the next step of a run.
/** Grandiose naming scheme (c) sjors dubya.
 *
 * After an event was instantiated, this function is called to explore the
 * remainder of the system.
 *
 * Note that some additional pruning is done, so this function
 * also determines the enabledness of the event that was just instantiated.
 *
 *@returns 1 (true) if the event was enabled.
 *\sa match_basic(),executeTry()
 */

int
explorify (const System sys, const int run)
{
  Roledef rd;
  int flag;
  int myStep;
  Roledef roleCap, roleCapPart;

  rd = runPointerGet (sys, run);
  myStep = sys->runs[run].step;
  roleCap = NULL;

  if (rd == NULL)
    {
      error ("Trying to progress completed run!\n");
    }

  flag = 0;

  /*
   * Special checks after (implicit) choose events; always first in run reads.
   */
  if (myStep == 0 && rd->type == READ)
    {
      int rid;

      if (inTermlist (sys->untrusted, agentOfRun (sys, run)))
	{
          /* this run is executed by an untrusted agent, do not explore */
          return 0;
	}
      /* executed by trusted agent */

      /* Special check 1: if agents have been instantiated in such a way that no more claims in any run
       * need to be evaluated, then we can skip
       * further traversal.
       */
      //!@todo This implementation relies on the fact that there are only secrecy, synchr and agreement properties.
      if (sys->switchNomoreClaims && sys->secrets == NULL)
	{ /* there are no remaining secrecy claims to be checked */
	  Roledef rdscan;
	  int validclaim;

	  rid = 0;
	  validclaim = 0;
	  /* check for each run */
	  while (rid < sys->maxruns)
	    { 
	      /* are claims in this run evaluated anyway? */
	      if (!untrustedAgent (sys, sys->runs[rid].agents))
		{ /* possibly claims to be checked in this run */
		  rdscan = runPointerGet(sys, rid);
		  while (rdscan != NULL)
		    {
		      if (rdscan->type == CLAIM)
			{
			  /* force abort of loop */
			  validclaim = 1;
			  rdscan = NULL;
			  rid = sys->maxruns;
			}
		      else
			{
			  rdscan = rdscan->next;
			}
		    }
		}
	      rid++;
	    }
	  if (validclaim == 0)
	    { /* no valid claims, abort */
	      return 0;
	    }
	}

      /* Special check 2: Symmetry reduction.
       * If the run we depend upon has already been activated (otherwise warn!) check for instance ordering
       */

      if (sys->switchAgentSymm && sys->runs[run].prevSymmRun != -1)
	{
	  /* there is such a run on which we depend */
	  int ridSymm;

	  ridSymm = sys->runs[run].prevSymmRun;
	  if (sys->runs[ridSymm].step == 0)
	    {
	      /*
	       * dependency run was not chosen yet, so we can't do anything now
	       */
	      // warning ("Symmetrical run dependency #%i (for run #%i) has not chosen yet!", ridSymm, run);
	    }
	  else
	    {
	      /* dependent run has chosen, so we can compare */
	      if (termlistOrder (sys->runs[run].agents,
				   sys->runs[ridSymm].agents) < 0)
		{
		  /* we only explore the other half */
		  return 0;
		}
	    }
	}

      /* Special check 3: if after choosing, this run ends on (read|skippedclaim)*, we can remove that part already.
       */

      if (sys->switchReduceEndgame)
       	  roleCap = removeIrrelevant (sys, run, rd);

      /* Special check x: if all agents in each run send only encrypted stuff, and all agents are trusted,
       * there is no way for the intruder to learn anything else than encrypted terms, so secrecy claims will not
       * be violated anymore if they contain no terms that are encrypted with such keys */

      //!@todo For now, there is no check that the runs only send publicly encrypted stuff! Just an assumption to be made true using static analysis.

      /*
      rid = 0;
      while (rid < sys->maxruns)
	{
	  if (!untrustedAgent (sys, sys->runs[rid].agents))
	    {
	    }
	  rid++;
	}
	*/
    }

  /* 
   * Special check b1: symmetry reduction part II on similar read events for equal roles.
   */

  if (sys->switchReadSymm)
    {
      if (sys->runs[run].firstNonAgentRead == myStep)
	{
	  /* Apparently, we have a possible ordering with our symmetrical friend.
	   * Check if it has progressed enough, and has the same agents.
	   */
	  int ridSymm;
     
	  if (rd->type != READ)
	    {
	      error ("firstNonAgentRead is not a read?!");
	    }
	  ridSymm = sys->runs[run].prevSymmRun;
	  if (isTermlistEqual (sys->runs[run].agents, sys->runs[ridSymm].agents))
	    {
	      /* same agents, so relevant */
	      if (myStep > 0 && sys->runs[ridSymm].step < myStep)
		{
		  // warning ("Symmetrical firstread dependency #%i (for run #%i) has not chosen yet!", ridSymm, run);
		}
	      else
		{
		  if (sys->runs[ridSymm].step <= myStep)
		    {
		      // warning ("Symmetrical firstread dependency #%i (for run #%i) has not read it's firstNonAgentRead %i yet, as it is only at %i!", ridSymm, run, myStep, sys->runs[ridSymm].step);
		    }
		  else
		    {
		      /* read was done, so we can compare them */
		      int i;
		      Roledef rdSymm;

		      rdSymm = sys->runs[ridSymm].start;
		      i = myStep;
		      while (i > 0)
			{
			  rdSymm = rdSymm->next;
			  i--;
			}
		      /* rdSymm now points to the instance of the symmetrical read */
		      i = termOrder (rdSymm->message, rd->message);
		      if (i < 0)
			{
			  /* only explore symmetrical variant */
			  return 0;
			}
		    }
		}
	    }
	}
    }

  /* Special check b2: symmetry order reduction.
   *
   * Concept: when there are two identical runs w.r.t. agents, we can make sure one goes before the other.
   * Depends on prevSymm, skipping chooses even.
   */

  if (sys->switchSymmOrder && myStep == sys->runs[run].firstReal)
    {
      if (sys->runs[run].prevSymmRun != -1)
	{
	  /* there is such a run on which we depend */
	  int ridSymm;

	  ridSymm = sys->runs[run].prevSymmRun;
	  /* equal runs? */

	  if (isTermlistEqual (sys->runs[run].agents, sys->runs[ridSymm].agents))
	    {
	      /* so, we have an identical partner */
	      /* is our partner there already? */
	      if (sys->runs[ridSymm].step <= myStep)
		{
		  /* not yet there, this is not a valid exploration */
		  /* verify !! */
		  return 0;
		}
	    }
	}
    }

  /* Apparently, all is well, and we can explore further */
  flag = 0;
  if (roleCap != NULL)
    {
      roleCapPart = roleCap->next;
      roleCap->next = NULL;
    }
  if (executeStep (sys, run))
    {
      /* traverse the system after the step */
      flag = traverse (sys);
    }
  /* restore executeStep "damage" */
  runPointerSet (sys, run, rd);	// reset rd pointer
  sys->runs[run].step = myStep;	// reset local index
  sys->step--;
  indentSet (sys->step);

  if (roleCap != NULL)
    {
      roleCap->next = roleCapPart;
    }
  return 1;	// The event was indeed enabled (irrespective of traverse!)
}

__inline__ int
traverseSimple (const System sys)
{
  /* simple nondeterministic traversal */

  int run;
  int flag = 0;

  for (run = 0; run < (sys->maxruns); run++)
    {
      if (runPointerGet (sys, run) != NULL)
	{
	  flag = 1;
	  executeTry (sys, run);
	}
    }
  if (!flag)
    {
      /* trace was not succesful */
    }
  return flag;
}

/*
 * nonReads
 *
 * Do a certain type of action first, i.e. that which satisfies nonRead(System,
 * Roledef). Use the inverse of this predicate to detect the other type of
 * event.
 */

#define predRead(sys,rd)	( rd->type == READ && !rd->internal )
#define isRead(sys,rd)		( rd != NULL && predRead(sys,rd) )
#define nonRead(sys,rd)		( rd != NULL && !predRead(sys,rd) )

__inline__ int
nonReads (const System sys)
{
  /* all sends first, then simple nondeterministic traversal */

  int run;
  Roledef rd;

  /* check for existence of executable sends */
  for (run = 0; run < (sys->maxruns); run++)
    {
      rd = runPointerGet (sys, run);
      if (nonRead (sys, rd))
	{
	  return executeTry (sys, run);
	}
    }
  return 0;
}

__inline__ int
traverseNonReads (const System sys)
{
  if (nonReads (sys))
    return 1;
  else
    return traverseSimple (sys);
}

int
traversePOR (const System sys)
{
  int flag = 0;
  int phase = sys->PORphase;
  int done = sys->PORdone;
  Roledef rd;

  if (phase == -1)
    {
      /* if we did nothing in the previous scan, this does not
         add anything new, and we exit. */
      if (done == 0)
	return 0;

      if (nonReads (sys))
	{
	  return 1;
	}
      else
	{
	  sys->PORphase = 0;
	  sys->PORdone = 0;
	  flag = traversePOR (sys);
	  sys->PORphase = phase;
	  sys->PORdone = done;
          return flag;
	}
    }
  else
    {
      /* other phase: branch the reads */

      if (phase == 0)
	{
	  /* phase 0: init the relevant counters */
	  sys->PORdone = 0;
	  flag = 0;
	}

      (sys->PORphase)++;
      if (sys->PORphase == sys->maxruns)
	{
	  sys->PORphase = -1;
	}

      rd = runPointerGet (sys, phase);
      if (isRead (sys, rd))
	{
	  /* consider both possibilities */

	  /* empty branch test */
	  if (sys->PORphase != -1 || sys->PORdone == 1)
	    {
	      /* apparently we have already done something,
	       * so we can consider _not_ doing this read */

	      /* option 1: we do not execute the event */

#ifdef DEBUG
	      if (DEBUGL (5))
		{
		  indent ();
		  printf
		    ("We are not executing a non-send event in phase %i\n",
		     phase);
		}
#endif
	      switch (sys->clp)
		{
		case 0:
		  flag = block_basic (sys, phase);
		  break;
		case 1:
		  flag = block_clp (sys, phase);
		  break;
		default:
		  fprintf (stderr, "Non existing clp switch.\n");
		  exit (1);
		}
	    }

	  /* option 2: we do execute the event */

#ifdef DEBUG
	  if (DEBUGL (5))
	    {
	      indent ();
	      printf
		("We are actually executing a non-send event in phase %i\n",
		 phase);
	    }
#endif
	  sys->PORdone = 1;	// mark that we did (try) stuff
	  flag = executeTry (sys, phase);
	}
      else
	{
	  /* empty branch test */
	  if (sys->PORphase != -1 || sys->PORdone == 1)
	    {
	      /* something else or null, proceed with scan */
	      flag = traverse (sys);
	    }
	}
      sys->PORdone = done;
      sys->PORphase = phase;
      return flag;
    }
}

/*
 * POR2b
 *
 * currently -t7
 *
 * New partial order reduction, which ought to be much more intuitive.
 */

__inline__ int
traversePOR2b (const System sys)
{
  Roledef runPoint;
  int flag = 0;
  int phase = sys->PORphase;
  int done = sys->PORdone;

  if (phase == -1)
    {
      /* if we did nothing in the previous scan, this does not
         add anything new, and we exit. */
      if (done == 0)
	{
#ifdef DEBUG
	  if (DEBUGL (5))
	    {
	      indent ();
	      printf
		("Read branch had no executed reads in phase %i, pruning tree.\n",
		 phase);
	    }
#endif
	  return 0;
	}

      /* nonReads first, as long as they exist */

      if (nonReads (sys))
	{
	  /* there was a nonread executed, branches itself */
	  return 1;
	}
      else
	{
	  /* no more nonreads, go to next phase */

	  sys->PORphase = 0;
	  sys->PORdone = 0;
	  flag = traverse (sys);
	  sys->PORdone = done;
	  sys->PORphase = phase;
	  return flag;
	}
    }
  else
    {
      /* other phase: branch the reads */

      (sys->PORphase)++;
      if (sys->PORphase == sys->maxruns)
	{
	  sys->PORphase = -1;
	}

      runPoint = runPointerGet (sys, phase);
      if (isRead (sys, runPoint))
	{

	  /* A read, we were looking for one of those.  Consider
	   * both possibilities */

	  /* option A. Try to execute the event */

#ifdef DEBUG
	  if (DEBUGL (5))
	    {
	      indent ();
	      printf ("We are actually executing a read event in phase %i\n",
		      phase);
	    }
#endif
	  sys->PORdone++;
	  flag = executeTry (sys, phase);
	  sys->PORdone--;

	  /* option B. Not execute now */

	  if (!flag)
	    {
	      /* It did not work now, so we try it later */
#ifdef DEBUG
	      if (DEBUGL (5))
		{
		  indent ();
		  printf ("Postponing a failed read in phase %i\n", phase);
		}
#endif

	      flag = traverse (sys);
	    }
	  else
	    {
	      /* It worked. Will the situation change later
	       * however?  Surely only if there was a
	       * variable involved, we might try it later */

	      if (hasTermVariable (runPoint->message))
		{
		  /* It has a variable, so we might try
		   * it later, but _only_ with different
		   * knowledge */

#ifdef DEBUG
		  if (DEBUGL (5))
		    {
		      indent ();
		      printf
			("Postponing a read for later ideas in phase %i\n",
			 phase);
		    }
#endif
		  if (!sys->clp)
		    {
		      /* non-clp */
		      flag = block_basic (sys, phase);
		    }
		  else
		    {
		      /* clp */
		      flag = block_clp (sys, phase);
		    }
		}
	      else
		{
		  /* no more hope */
#ifdef DEBUG
		  if (DEBUGL (5))
		    {
		      indent ();
		      printf ("No more hope in phase %i.\n", phase);
		    }
#endif
		}
	    }

	}
      else
	{
	  /* it is not a read. this actually should mean the role is empty */

	  /* something else or null, proceed with scan */
	  flag = traverse (sys);
	}

      /* reset phase counter */
      sys->PORphase = phase;
      return flag;
    }
}

__inline__ int
traversePOR2 (const System sys)
{
  Roledef runPoint;
  int flag = 0;
  int phase = sys->PORphase;
  int done = sys->PORdone;

  if (phase == -1)
    {
      /* if we did nothing in the previous scan, this does not
         add anything new, and we exit. */
      if (sys->PORdone == 0)
	return 0;

      if (nonReads (sys))
	{
	  return 1;
	}
      else
	{
	  sys->PORphase = 0;
	  sys->PORdone = 0;
	  flag = traversePOR2 (sys);
	  sys->PORphase = phase;
	  sys->PORdone = done;
	  return flag;
	}
    }
  else
    {
      /* other phase: branch the reads */

      if (phase == 0)
	{
	  /* phase 0: init the relevant counters */
	  sys->PORdone = 0;
	  flag = 0;
	}

      (sys->PORphase)++;
      if (sys->PORphase == sys->maxruns)
	{
	  sys->PORphase = -1;
	}

      runPoint = runPointerGet (sys, phase);

      if (isRead (sys, runPoint))
	{
	  /* empty branch test */
	  if (sys->PORphase != -1 || sys->PORdone == 1)
	    {
	      /* option 1: we do not execute the event */

#ifdef DEBUG
	      if (DEBUGL (5))
		{
		  indent ();
		  printf
		    ("We are not executing a non-send event in phase %i\n",
		     phase);
		}
#endif
	      switch (sys->clp)
		{
		case 0:

		  /* the idea is, that if the
		   * read has no variables, and
		   * is enabled already, it makes
		   * no sense to delay it any
		   * longer. */

		  if (hasTermVariable (runPoint->message))
		    {
		      /* original case */
		      flag = block_basic (sys, phase);
		    }
		  else
		    {
		      /* check whether it was enabled already */
		      /* HACK this is a custom 'enabled' test. */
		      /* TODO rewrite this, we don't want such a test. */
		      if (!inKnowledge (sys->know, runPoint->message))
			{
			  /* not enabled yet, so we might want to do so later */
			  flag = block_basic (sys, phase);
			}
		      else
			{
			  /* enabled, skipping */
			}
		    }
		  break;
		case 1:
		  flag = block_clp (sys, phase);
		  break;
		default:
		  fprintf (stderr, "Non existing clp switch.\n");
		  exit (1);
		}
	    }

	  /* option 2: we do execute the event */

#ifdef DEBUG
	  if (DEBUGL (5))
	    {
	      indent ();
	      printf
		("We are actually executing a non-send event in phase %i\n",
		 phase);
	    }
#endif
	  sys->PORdone = 1;	// mark that we did (try) stuff
	  flag = executeTry (sys, phase);
	}
      else
	{
	  /* empty branch test */
	  if (sys->PORphase != -1 || sys->PORdone == 1)
	    {
	      /* something else or null, proceed with scan */
	      flag = traverse (sys);
	    }
	}
      sys->PORdone = done;
      sys->PORphase = phase;
      return flag;
    }
}

__inline__ int
traversePOR3 (const System sys)
{
  Roledef rd;
  int flag;
  int run;

  if (nonReads (sys))
    {
      return 1;
    }

  for (run = 0; run < sys->maxruns; run++)
    {
      rd = runPointerGet (sys, run);
      if (rd != NULL)
	{
	  /* option 1: just execute it */

	  flag = executeTry (sys, run);

	  /* option 2: if it worked, and its a read... */

	  if (flag)
	    {
	      if (rd->type == READ &&
		  !(rd->internal) && hasTermVariable (rd->message))
		{

		  /* option 2a: worked, but also execute
		   * it later if we thinks that's
		   * different actually only relevant for
		   * non internal reads */

		  /* TODO consider option for global
		   * 'sendsTodo' counter, because if
		   * there are zero left, this is not
		   * required. */

#ifdef DEBUG
		  if (DEBUGL (5))
		    {
		      indent ();
		      printf ("Blocking read for run #%i.\n", run);
		    }
#endif
		  block_basic (sys, run);
		}
	      return 1;
	    }
	}
    }
  return 0;
}

//! Execute a read in the knowledge postponing way, on which the partial order reduction for secrecy depends.

__inline__ int
tryChoiceRead (const System sys, const int run, const Roledef rd)
{
  int flag;
  
  flag = 0;

  /* the sendsdone check only prevent
   * some unneccessary inKnowledge tests,
   * and branch tests, still improves
   * about 15% */
  if (sys->knowPhase > rd->knowPhase)
    {
      /* apparently there has been a new knowledge item since the
       * previous check */

      /* implicit check for enabledness */
      flag = executeTry (sys, run);

      /* if it was enabled (flag) we postpone it if it makes sense
       * to do so (hasVariable, non internal) */
      if (flag && hasTermVariable (rd->message) && !rd->internal)
	{
	  int stackKnowPhase = rd->knowPhase;

	  rd->knowPhase = sys->knowPhase;
	  if (sys->clp)
	    {
	      block_clp (sys, run);
	    }
	  else
	    {
	      block_basic (sys, run);
	    }
	  rd->knowPhase = stackKnowPhase;
	}
    }
  return flag;
}

//! Try to execute the event at the roledef. Returns true iff it was enabled, and thus explored.
/**
 * Note that rd should not be NULL
 */
__inline__ int
tryChoiceRoledef (const System sys, const int run, const Roledef rd)
{
  int flag;

#ifdef DEBUG
  if (rd == NULL)
      error ("tryChoiceRoledef should not be called with a NULL rd pointer");
#endif
  
  flag = 0;
  switch (rd->type)
    {
    case CLAIM:
    case SEND:
      flag = executeTry (sys, run);
      break;

    case READ:
      flag = tryChoiceRead (sys, run, rd);
      break;

    default:
      fprintf (stderr, "Encountered unknown event type %i.\n", rd->type);
      exit (1);
    }
  return flag;
}

//! Try to execute the event in a given run
__inline__ int
tryChoiceRun (const System sys, const int run)
{
  Roledef rd;

  rd = runPointerGet (sys, run);
  if (rd != NULL)
      return tryChoiceRoledef (sys, run, rd);
  else
      return 0;
}

//! Yield the last active run in the partial trace, or 0 if there is none.
__inline__ int
lastActiveRun (const System sys)
{
  if (sys->step == 0)
    {
      /* first step, start at 0 */
      return 0;
    }
  else
    {
      /* there was a previous action, start scan from there */
      return sys->traceRun[sys->step - 1] + sys->porparam;
    }
}

//! Determine whether a roleevent is a choose event
__inline__ int
isChooseRoledef (const System sys, const int run, const Roledef rd)
{
  return (rd == sys->runs[run].start &&
	  rd->type == READ && 
	  rd->internal);
}

//! Explore possible chooses first
__inline__ int
tryChoosesFirst (const System sys)
{
  int flag;
  int run;
  Roledef rd;

  flag = 0;
  for (run = 0; run < sys->maxruns && !flag; run++)
    {
      rd = runPointerGet (sys, run);
      if (isChooseRoledef (sys, run, rd))
	  flag = tryChoiceRoledef (sys, run, rd);
    }
  return flag;
}

//! Explore left-to-right (from an offset)
__inline__ int
tryEventsOffset (const System sys, const int offset)
{
  int flag;
  int run;
  int i;

  flag = 0;
  for (i = 0; i < sys->maxruns && !flag; i++)
    {
      run = (i + offset) % sys->maxruns;
      flag = tryChoiceRun (sys, run);
    }
  return flag;
}

/*
 * POR4
 *
 * This is the simplified version of the algorithm, to be compared with
 * the -t7 version.
 *
 * Based on some new considerations.
 */

__inline__ int
traversePOR4 (const System sys)
{
  return tryEventsOffset (sys, lastActiveRun (sys));
}

/*
 * POR5
 *
 * POR4 but does chooses first.
 */

__inline__ int
traversePOR5 (const System sys)
{
  if (tryChoosesFirst (sys))
      return 1;
  return tryEventsOffset (sys, lastActiveRun (sys));
}

/*
 * POR6
 *
 * POR5 but has a left-oriented scan instead of working from the current run.
 */

__inline__ int
traversePOR6 (const System sys)
{
  return tryEventsOffset (sys, 0);
}

/*
 * POR7
 *
 * Left-oriented scan, to ensure reductions. However, first does all initial actions.
 */

__inline__ int
traversePOR7 (const System sys)
{
  if (tryChoosesFirst (sys))
      return 1;
  tryEventsOffset (sys, 0);
}

/*
 * POR8
 *
 * POR6, but tries to continue on the current run first. This turned out to be highly more efficient.
 */

__inline__ int
traversePOR8 (const System sys)
{
  int flag;
  int run;
  int last;

  last = lastActiveRun (sys);
  flag = tryChoiceRun (sys, last);
  for (run = 0; run < sys->maxruns && !flag; run++)
    {
      if (run != last)
          flag = tryChoiceRun (sys, run);
    }
  return flag;
}


//! Check for the properties that have lasting effects throughout the trace.
/**
 * Currently, only functions for secrecy.
 */

int
propertyCheck (const System sys)
{
  int flag = 1;	// default: properties are true, no attack 

  /* for now, we only check secrecy */
  if (sys->secrets != NULL)
    {
      Termlist scan;
      scan = sys->secrets;
      while (scan != NULL)
	{
	  if (!claimSecrecy (sys, scan->term))
	    {
	      /* apparently, secrecy of this term was violated */
	      /* find the violated claim event */

	      Termlist tl = NULL;
	      int claimev = -1;
	      int i = 0;

	      while (claimev == -1 && i <= sys->step)
		{
		  if (sys->traceEvent[i]->type == CLAIM &&
		      sys->traceEvent[i]->to == CLAIM_Secret)
	 	    {
		      Termlist tl = secrecyUnfolding(scan->term, sys->know);
		      if (tl != NULL)
		        {
		          /* This was indeed a violated claim */
			  claimev = i;
		        }
		    }
		  i++;
		}
	      /* do we have it? */
	      if (claimev == -1)
		{
		  /* weird, should not occur */
		  fprintf(stderr, "Violation, but cannot locate claim.\n");
		  printf("A secrecy claim was supposed to be violated on term ");
		  termPrint(scan->term);
		  printf(" but we couldn't find the corresponding claim.\n");
		  exit(1);
		}
	      else
		{
		  /* fine. so it's violated */
		  violateClaim(sys, sys->step, claimev, tl);
		  termlistDelete(tl);
		  flag = 0;
		}
	    }
	  scan = scan->next;
	}

    }
  return flag;
}

/*	true iff the term is secret */

int
isTermSecret (const System sys, const Term t)
{
  switch (sys->clp)
    {
    case 0:
      /* test for simple inclusion */
      if (inKnowledge (sys->know, t))
	return 0;
      if (isTermVariable (t))
	{
	  /* it's a variable! */

	  /* TODO that does not necessarily mean we can choose it, does
	   * it?  NO: the rule should be: there is at least one message
	   * in knowledge. We don't check it currently.*/

	  return 0;
	}
      return 1;
    case 1:
      /* CLP stuff */
      return secret_clp (sys, t);
    default:
      return 0;
    }
}

/*	true iff the claim is valid */

int
claimSecrecy (const System sys, const Term t)
{
  int csScan (Term t)
  {
    t = deVar (t);
    if (isTermTuple (t))
      return csScan (t->left.op1) && csScan (t->right.op2);
    else
      return isTermSecret (sys, t);
  }

  if (csScan (t))
    return 1;
  else
    {
      /* Not reported anymore here, but only at the end */
      // reportSecrecy (sys, t);
      return 0;
    }
}

/*
 * Unfold the secrecy tuple and construct a list of terms that violate it.
 */

Termlist
secrecyUnfolding (Term t, const Knowledge know)
{
  t = deVar (t);
  if (isTermTuple (t))
    return termlistConcat (secrecyUnfolding(t->left.op1,know),
			   secrecyUnfolding(t->right.op2,know)
			  );
  else
    {
      if (inKnowledge(know, t))
	  return termlistAdd(NULL, t);
      else
	  return NULL;
    }
}

/*
 * for reporting we need a more detailed output of the claims.
 * Output is a termlist pointer, or -1.
 *
 * in: claim roledef, knowledge for which it is violated
 *
 * -1	: claim was ignored
 * NULL : claim is fulfilled (true)
 * Termlist: claim was violated, termlist terms are know to the intruder.
 */

Termlist
claimViolationDetails (const System sys, const int run, const Roledef rd, const Knowledge know)
{
  if (rd->type != CLAIM)
    {
      fprintf(stderr, "Trying to determine details of something other than a claim!\n");
      exit(-1);
    }

  /* cases */
  if (rd->to == CLAIM_Secret)
    {
      /* secrecy claim */
      
      if (untrustedAgent (sys, sys->runs[run].agents))
	{
	  /* claim was skipped */
	  return (Termlist) -1;
	}
      else
	{
	  /* construct violating subterms list */
          return secrecyUnfolding(rd->message, know);
	}
    }
  return NULL;
}

//! A claim was violated.
/**
 * This happens when we violate a claim.
 * Lots of administration.
 *@returns True iff explorify is in order.
 */
int
violateClaim (const System sys, int length, int claimev, Termlist reqt)
{
  int flag;

  /* default = no adaption of pruning, continue search */
  flag = 1;

  /* Count the violations */
  sys->failed = statesIncrease (sys->failed);

  /* mark the path in the state graph? */
  if (sys->switchStatespace)
    {
      graphPath (sys, length);
    }

  /* Copy the current trace to the buffer, if the new one is shorter than the previous one. */
  if (sys->attack == NULL || length < sys->attack->reallength)
    {
      tracebufDone(sys->attack);
      sys->attack = tracebufSet(sys, length, claimev);
      attackMinimize (sys, sys->attack);
      sys->shortestattack = sys->attack->reallength;

      /* maybe there is some new pruning going on */
      flag = 0;
      switch (sys->prune)
	{
	case 0:
	  flag = 1;
	  break;
	case 1:
	  break;
	case 2:
	  sys->maxtracelength = sys->shortestattack - 1;
	  break;
	} 
    }
  return flag;
}

//! Try to execute the next event in a run.
/**
 * One of the core functions of the system.
 *@returns 0 (false) if the event was not enabled. 1 (true) if there was an enabled instantiation of this event.
 */
int
executeTry (const System sys, int run)
{
  Roledef runPoint;
  int flag = 0;

  runPoint = runPointerGet (sys, run);
  sys->traceEvent[sys->step] = runPoint;	// store for later usage, problem: variables are substituted later...
  sys->traceRun[sys->step] = run;		// same

  if (runPoint == NULL)
    {
#ifdef DEBUG
      /* warning, ought not to occur */
      debug (2, "Trying to activate completed run");
#endif
    }
  else
    {
#ifdef DEBUG
      if (DEBUGL (4))
	{
	  indent ();
	  printf ("try: ");
	  roledefPrint (runPoint);
	  printf ("#%i\n", run);
	}
#endif
      if (runPoint->type == READ)
	{
	  if (sys->clp)
	    return matchRead_clp (sys, run, explorify);
	  else
	    return matchRead_basic (sys, run, explorify);
	}
      if (runPoint->type == SEND)
	{
	  if (sys->clp)
	    flag = send_clp (sys, run);
	  else
	    flag = send_basic (sys, run);
	  return flag;
	}

      /*
       * Execute claim event
       */
      if (runPoint->type == CLAIM)
	{
	  /* first we might dynamically determine whether the claim is valid */
	  if (untrustedAgent (sys, sys->runs[run].agents))
	    {
	      /* for untrusted agents we check no claim violations at all
	       * so: we know it's okay. */
	      /* TODO for CLP this doesn't work and call for branching, if the
	       * agent is a variable */
#ifdef DEBUG
	      if (DEBUGL (3))
		{
		  indent ();
		  printf ("Skipped claim in untrusted run with agents ");
		  termlistPrint (sys->runs[run].agents);
		  printf ("\n");
		}
#endif
	      explorify (sys, run);
	      return 1;
	    }

	  /* determine type of claim, and parameters */
#ifdef DEBUG
	  if (DEBUGL (2))
	    {
	      indent ();
	      printf ("claim: ");
	      roledefPrint (runPoint);
	      printf ("#%i\n", run);
	    }
#endif
	  /*
	   * update claim counters
	   */
	  sys->claims = statesIncrease (sys->claims);

	  /*
	   * distinguish claim types
	   */
	  if (runPoint->to == CLAIM_Secret)
	    {
	      /*
	       * SECRECY
	       */
	      /* TODO claims now have their own type, test for that */
	      /* TODO for now it is secrecy of the message */

	      Termlist oldsecrets = sys->secrets;
	      /* TODO this can be more efficient, by filtering out double occurrences */
	      sys->secrets =
		termlistAdd (termlistShallow (oldsecrets), runPoint->message);
	      flag = claimSecrecy (sys, runPoint->message);
              runPoint->claiminfo->count++;

	      /* now check whether the claim failed for further actions */
	      if (!flag)
		{
		  /* violation */
		  Termlist tl;

                  runPoint->claiminfo->failed++;
		  tl = claimViolationDetails(sys,run,runPoint,sys->know);
		  if (violateClaim (sys,sys->step+1, sys->step, tl ))
		      flag = explorify (sys, run);
		  termlistDelete(tl);
		}
	      else
		{
		  /* no violation */
		  flag = explorify (sys, run);
		}

	      /* reset secrets list */
	      termlistDelete (sys->secrets);
	      sys->secrets = oldsecrets;
	    }
	  if (runPoint->to == CLAIM_Nisynch)
	    {
	      /*
	       * NISYNCH
	       */
	      //!@todo TODO nisynch implementation

              flag = check_claim_nisynch (sys, sys->step);
	      if (!flag)
		{
		  /* violation */
		  if (violateClaim (sys,sys->step+1, sys->step, NULL ))
		      flag = explorify (sys, run);
		}
	      else
		{
		  /* no violation */
		  flag = explorify (sys, run);
		}
	    }
	}
      /* a claim always succeeds */
      flag = 1;
    }
  return flag;
}


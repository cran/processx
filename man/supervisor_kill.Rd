% Generated by roxygen2: do not edit by hand
% Please edit documentation in R/supervisor.R
\name{supervisor_kill}
\alias{supervisor_kill}
\title{Terminate all supervised processes and the supervisor process itself as
well}
\usage{
supervisor_kill()
}
\description{
On Unix the supervisor sends a \code{SIGTERM} signal to all supervised
processes, and gives them five seconds to quit, before sending a
\code{SIGKILL} signal. Then the supervisor itself terminates.
}
\details{
Windows is similar, but instead of \code{SIGTERM}, a console CTRL+C interrupt
is sent first, then a \code{WM_CLOSE} message is sent to the windows of the
supervised processes, if they have windows.
}
\keyword{internal}

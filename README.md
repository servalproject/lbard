# lbard
Low-bandwidth Asynchronous Rhizome Demonstrator

Communicates with a running servald process using the HTTP based
RESTful API.  This means that you need to setup an authentication
username and password, e.g.:

./servald config set api.restful.users.lbard.password lbard

This creates an authorisation for username lbard with password lbard.

These credentials then need to be provided to lbard so that it can
access the HTTP API.

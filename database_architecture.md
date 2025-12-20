# Database architecture guide

## Core requirements

- Global connection pool
- Database management object injected into any component requiring database access
- Encapsulate complexity of connection pool management
- Monitoring and log alert of resource shortage in connection pool

## Advanced features

### Replication and local fail-over

To accommodate local on-premises operation, there needs to be an option
to run a local instance of Postgres in slave replication mode. In connectivity
to the cloud servers running the shared Postgres database is lost files stored
locally must still be accessible, though in read-only mode.

The local copy of Postgres is replicating the master in the cloud. In the
event that connectivity is lost FileEngine need to switch to the local database,
but operate in read-only mode until connectivity restored.

An outage needs to be dealt with in a simular way to an S3 outage. There needs
to be sentinel thread checking for primary database connectivity and 
perform retry on heartbeat.



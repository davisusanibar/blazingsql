package com.blazingdb.calcite.catalog.domain;

import org.hibernate.annotations.Cascade;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.FetchType;
import javax.persistence.GeneratedValue;
import javax.persistence.Id;
import javax.persistence.JoinColumn;
import javax.persistence.ManyToOne;
import javax.persistence.MapKey;
import javax.persistence.OneToMany;
import javax.persistence.Table;

/**
 * <h1>Stores a table with its corresponding columns</h1>
 * This domain class is a table and the columns inside the table.
 * It is used by hibernate to persist tables.
 *
 * @author felipe
 *
 */
@Entity
@Table(name = "blazing_catalog_tables")
public class CatalogTableImpl implements CatalogTable {
	/**
	 * Empty constructor just initializes the map for columns.
	 */
	public CatalogTableImpl() { this.tableColumns = new HashMap<String, CatalogColumnImpl>(); }

	/**
	 * This constructor is used to fill in all the values for the table.
	 *
	 * @param name the name of the table that is being created
	 * @param db the database this table is being added to
	 * @param columns list of columns to be added to the table.
	 */
	public CatalogTableImpl(String name, CatalogDatabaseImpl db, List<CatalogColumnImpl> columns) {
		this.name = name;
		this.database = db;
		this.tableColumns = new HashMap<String, CatalogColumnImpl>();
		for(CatalogColumnImpl column : columns) {
			column.setTable(this);
			this.tableColumns.put(column.getColumnName(), column);
		}
	}

	/**
	 * This constructor is used to fill in all the values for the table.
	 *
	 * @param name the name of the table that is being created
	 * @param db the database this table is being added to
	 * @param columns list of columns to be added to the table.
	 */
	public CatalogTableImpl(String name, CatalogDatabaseImpl db, List<CatalogColumnImpl> columns, int rowcount) {
		this.name = name;
		this.database = db;
		this.tableColumns = new HashMap<String, CatalogColumnImpl>();
		for(CatalogColumnImpl column : columns) {
			column.setTable(this);
			this.tableColumns.put(column.getColumnName(), column);
		}
		this.rowcount = Double.valueOf(rowcount);
	}

	/**
	 * This constructor is used to fill in all the values for the table.
	 * @param name the name of the table that is being created
	 * @param db the database this table is being added to
	 * @param columnNames a list of strings with column names
	 * @param columnTypes a list of strings that map to {@link
	 *     com.blazingdb.calcite.catalog.domain.CatalogColumDataType}
	 */
	public CatalogTableImpl(String name, CatalogDatabaseImpl db, List<String> columnNames, List<String> columnTypes) {
		this.name = name;
		this.database = db;
		this.tableColumns = new HashMap<String, CatalogColumnImpl>();
		for(int i = 0; i < columnNames.size(); i++) {
			CatalogColumnImpl column = new CatalogColumnImpl();
			column.setColumnDataType(columnTypes.get(i));
			column.setTable(this);
			column.setColumnName(columnNames.get(i));
			column.setOrderValue(i);
			this.tableColumns.put(column.getColumnName(), column);
		}
	}
	/**
	 * A value generated by the database which hibernate is using to persist schemas.
	 */
	@Id @GeneratedValue @Column(name = "id") private Long id;
	/**
	 * The name of the table.
	 */
	@Column(name = "name", nullable = false) private String name;
	/**
	 * The number of row of the table.
	 */
	@Column(name = "rowcount", nullable = false) private Double rowcount;

	/**
	 * A map of name to {@see CatalogColumnImpl}
	 */
	@OneToMany(fetch = FetchType.EAGER, mappedBy = "table", orphanRemoval = true)
	@Cascade({org.hibernate.annotations.CascadeType.ALL})
	@MapKey(name = "name")  // here this is the column name inside of CatalogColumn
	private Map<String, CatalogColumnImpl> tableColumns;

	/**
	 * The {@see CatalogDatabaseImpl} this table belongs to.
	 */
	@ManyToOne(fetch = FetchType.EAGER) @JoinColumn(name = "database_id") private CatalogDatabaseImpl database;

	@Override
	public CatalogDatabaseImpl
	getDatabase() {
		return database;
	}

	public void
	setDatabase(CatalogDatabaseImpl database) {
		this.database = database;
	}

	public Long
	getId() {
		return this.id;
	}

	public void
	setId(Long id) {
		this.id = id;
	}

	@Override
	public String
	getTableName() {
		return this.name;
	}

	public void
	setTableName(String name) {
		this.name = name;
	}

	@Override
	public Double getRowCount() {
		return this.rowcount;
	}

	public void setRowcount(Double rowcount) {
		this.rowcount = rowcount;
	}

	/**
	 * Converts the columns map into a Set of columns
	 * @return the set of {@see CatalogColumn} that belong to this table
	 */
	@Override
	public Set<CatalogColumn>
	getColumns() {
		List<CatalogColumnImpl> cols = new ArrayList<CatalogColumnImpl>();

		for(CatalogColumnImpl col : this.tableColumns.values()) {
			cols.add(col);
		}
		Collections.sort(cols);

		Set<CatalogColumn> tempColumns = new LinkedHashSet<CatalogColumn>();

		for(CatalogColumnImpl col : cols) {
			tempColumns.add(col);
		}

		return tempColumns;
	}

	public Map<String, CatalogColumnImpl>
	getTableColumns() {  // i think hibernate needs a getter of the private data
						 // type not sure about this
		return tableColumns;
	}

	public void
	setTableColumns(Map<String, CatalogColumnImpl> columns) {
		this.tableColumns = columns;
	}
}
